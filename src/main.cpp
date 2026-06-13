#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <AccelStepper.h>

// ---------------------------------------------------------------------------
// Pin configuration for NodeMCU v3 -> Stepper driver (A4988 / DRV8825 / TMC22xx)
//
//   NodeMCU pin   GPIO     Driver pin
//   -----------   ----     ----------
//   D1            GPIO5    STEP
//   D2            GPIO4    DIR
//   D3            GPIO0    ENABLE  (active LOW on most drivers)
//   D5            GPIO14   LIMIT switch / homing (optional, INPUT_PULLUP)
//
// Avoid D0/GPIO16 (no interrupt, used for wake), D8/GPIO15 (must be LOW at boot)
// and D3/GPIO0 + D4/GPIO2 if you need a clean boot (they have boot strap roles).
// GPIO0 is acceptable for ENABLE because the driver is HIGH-Z while booting.
// ---------------------------------------------------------------------------
constexpr uint8_t PIN_STEP   = D1;
constexpr uint8_t PIN_DIR    = D2;
constexpr uint8_t PIN_ENABLE = D3;
constexpr uint8_t PIN_LIMIT  = D5;

// ---------------------------------------------------------------------------
// Motion configuration
// ---------------------------------------------------------------------------
// Steps per millimetre of travel on the linear guide.
//   Example:  (400 steps/rev) + 10 mm/rev
//   lead screw  =>  400 / 10 = 400 steps/mm
constexpr float STEPS_PER_MM = 40.0f;

// Maximum travel of the guide in millimetres (soft limit).
constexpr float MAX_TRAVEL_MM = 1000.0f;

// Default motion profile.
constexpr float DEFAULT_SPEED_MM_S = 100.0f;   // mm/s
constexpr float DEFAULT_ACCEL_MM_S2 = 100.0f; // mm/s^2

// Driver enable is typically active HIGH.
constexpr uint8_t ENABLE_ACTIVE = HIGH;
constexpr uint8_t ENABLE_INACTIVE = LOW;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);
ESP8266WebServer server(80);

bool driverEnabled = false;

// Latched when the limit switch (assumed at the min-end, position 0) triggers.
// While latched, motion toward the switch is refused; only moves to a strictly
// larger position are accepted. The latch is cleared by /home or by a /jog in
// the positive direction once the switch reads "clear".
bool limitTripped = false;

void enableDriver(bool on) {
    digitalWrite(PIN_ENABLE, on ? ENABLE_ACTIVE : ENABLE_INACTIVE);
    driverEnabled = on;
}

long mmToSteps(float mm) {
    return static_cast<long>(mm * STEPS_PER_MM);
}

float stepsToMm(long steps) {
    return static_cast<float>(steps) / STEPS_PER_MM;
}

// Debounced read of the limit switch (active LOW with INPUT_PULLUP).
// Returns true if the switch is currently triggered.
bool limitSwitchTriggered() {
    if (digitalRead(PIN_LIMIT) != LOW) return false;
    delayMicroseconds(500);
    return digitalRead(PIN_LIMIT) == LOW;
}

// Hard halt: cancel any pending motion immediately (no decel) and latch.
// AccelStepper::stop() only schedules a decel, which is unsafe when we have
// already crashed into an end-stop.
void hardHalt() {
    stepper.moveTo(stepper.currentPosition());
    stepper.setSpeed(0);
}

// ---------------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Linear Guide</title>
  <style>
    body{font-family:sans-serif;margin:2em;max-width:480px}
    button,input{font-size:1em;padding:.4em .6em;margin:.2em}
    .row{display:flex;gap:.4em;flex-wrap:wrap}
    #status{margin-top:1em;padding:.6em;background:#eef;border-radius:6px;white-space:pre}
  </style>
</head>
<body>
  <h1>Linear Guide Control</h1>
  <div class="row">
    <button onclick="cmd('/enable')">Enable</button>
    <button onclick="cmd('/disable')">Disable</button>
    <button onclick="cmd('/stop')">STOP</button>
    <button onclick="cmd('/home')">Home (zero)</button>
  </div>
  <h3>Move to absolute position (mm)</h3>
  <div class="row">
    <input id="pos" type="number" step="0.1" value="0">
    <button onclick="cmd('/move?pos=' + document.getElementById('pos').value)">Go</button>
  </div>
  <h3>Jog (mm)</h3>
  <div class="row">
    <button onclick="cmd('/jog?d=-10')">-10</button>
    <button onclick="cmd('/jog?d=-1')">-1</button>
    <button onclick="cmd('/jog?d=-0.1')">-0.1</button>
    <button onclick="cmd('/jog?d=0.1')">+0.1</button>
    <button onclick="cmd('/jog?d=1')">+1</button>
    <button onclick="cmd('/jog?d=10')">+10</button>
  </div>
  <h3>Speed / Accel (mm/s, mm/s^2)</h3>
  <div class="row">
    <input id="spd" type="number" step="1" value="100">
    <input id="acc" type="number" step="1" value="100">
    <button onclick="cmd('/profile?speed=' + document.getElementById('spd').value + '&accel=' + document.getElementById('acc').value)">Apply</button>
  </div>
  <div id="status">loading...</div>
  <script>
    async function cmd(url){await fetch(url);refresh();}
    async function refresh(){
      const r = await fetch('/status');
      document.getElementById('status').textContent = await r.text();
    }
    setInterval(refresh, 500);
    refresh();
  </script>
</body>
</html>
)HTML";

void handleRoot() {
    server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
    String s;
    s += "enabled : " + String(driverEnabled ? "yes" : "no") + "\n";
    s += "position: " + String(stepsToMm(stepper.currentPosition()), 3) + " mm\n";
    s += "target  : " + String(stepsToMm(stepper.targetPosition()), 3) + " mm\n";
    s += "running : " + String(stepper.isRunning() ? "yes" : "no") + "\n";
    s += "speed   : " + String(stepper.maxSpeed() / STEPS_PER_MM, 2) + " mm/s\n";
    s += "accel   : " + String(stepper.acceleration() / STEPS_PER_MM, 2) + " mm/s^2\n";
    s += "limit   : " + String(digitalRead(PIN_LIMIT) == LOW ? "TRIGGERED" : "clear") + "\n";
    s += "latched : " + String(limitTripped ? "yes (move + to clear)" : "no") + "\n";
    server.send(200, "text/plain", s);
}

void handleEnable()  { enableDriver(true);  server.send(200, "text/plain", "ok"); }
void handleDisable() { stepper.stop(); enableDriver(false); server.send(200, "text/plain", "ok"); }
void handleStop()    { stepper.stop(); server.send(200, "text/plain", "ok"); }

void handleHome() {
    // Treat current position as zero and clear any limit-switch latch. Replace
    // with a real homing routine that drives toward PIN_LIMIT until triggered
    // if you have an end-stop installed.
    stepper.setCurrentPosition(0);
    limitTripped = false;
    server.send(200, "text/plain", "ok");
}

void handleMove() {
    if (!server.hasArg("pos")) { server.send(400, "text/plain", "missing pos"); return; }
    float mm = server.arg("pos").toFloat();
    mm = constrain(mm, 0.0f, MAX_TRAVEL_MM);
    long targetSteps = mmToSteps(mm);

    // If the limit (min-end) is latched, refuse any move that does not strictly
    // retreat from the switch.
    if (limitTripped && targetSteps <= stepper.currentPosition()) {
        server.send(409, "text/plain", "limit latched: jog + to clear or /home");
        return;
    }
    if (!driverEnabled) enableDriver(true);
    stepper.moveTo(targetSteps);
    server.send(200, "text/plain", "ok");
}

void handleJog() {
    if (!server.hasArg("d")) { server.send(400, "text/plain", "missing d"); return; }
    float delta = server.arg("d").toFloat();
    float target = stepsToMm(stepper.currentPosition()) + delta;
    target = constrain(target, -MAX_TRAVEL_MM, MAX_TRAVEL_MM);
    long targetSteps = mmToSteps(target);

    if (limitTripped) {
        // Only allow positive (away-from-switch) jogs, and only once the
        // switch has physically released, to avoid grinding back into it.
        if (delta <= 0.0f) {
            server.send(409, "text/plain", "limit latched: only positive jog allowed");
            return;
        }
        if (limitSwitchTriggered()) {
            server.send(409, "text/plain", "limit still triggered, release switch first");
            return;
        }
        limitTripped = false;
    }
    if (!driverEnabled) enableDriver(true);
    stepper.moveTo(targetSteps);
    server.send(200, "text/plain", "ok");
}

void handleProfile() {
    float speed = server.hasArg("speed") ? server.arg("speed").toFloat() : DEFAULT_SPEED_MM_S;
    float accel = server.hasArg("accel") ? server.arg("accel").toFloat() : DEFAULT_ACCEL_MM_S2;
    stepper.setMaxSpeed(speed * STEPS_PER_MM);
    stepper.setAcceleration(accel * STEPS_PER_MM);
    server.send(200, "text/plain", "ok");
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println(F("Linear guide controller starting...")); Serial.println(F("Debug: Firmware version 1.0"));

    pinMode(PIN_ENABLE, OUTPUT);
    enableDriver(false);
    pinMode(PIN_LIMIT, INPUT_PULLUP);

    stepper.setMaxSpeed(DEFAULT_SPEED_MM_S * STEPS_PER_MM);
    stepper.setAcceleration(DEFAULT_ACCEL_MM_S2 * STEPS_PER_MM);
    stepper.setPinsInverted(true, false, false);
    // NOTE: The enable line is owned exclusively by enableDriver() / digitalWrite
    // on PIN_ENABLE. We deliberately do NOT call stepper.setEnablePin() so
    // AccelStepper never toggles the pin on its own (which would race with our
    // manual control and could leave the driver disabled or stuck enabled).

    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.hostname("fsk40-linear-drive"); 
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to %s", WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
if (WiFi.status() == WL_CONNECTED) {
        Serial.println(F("Debug: WiFi connected"));
        Serial.print(F("IP: "));
        Serial.println(WiFi.localIP());
        Serial.print(F("Debug: MAC: "));
        Serial.println(WiFi.macAddress());
        Serial.print(F("Debug: RSSI: "));
        Serial.print(WiFi.RSSI());
        Serial.println(F(" dBm"));
    } else {
        Serial.println(F("Debug: WiFi connection failed"));
        Serial.println(F("WiFi not connected, continuing without network."));
    }

    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/enable", handleEnable);
    server.on("/disable", handleDisable);
    server.on("/stop", handleStop);
    server.on("/home", handleHome);
    server.on("/move", handleMove);
    server.on("/jog", handleJog);
    server.on("/profile", handleProfile);
    server.begin();
    Serial.println(F("HTTP server started"));

    ArduinoOTA.setHostname("fsk40-linear-drive");

    ArduinoOTA.onStart([]() {
        Serial.println(F("OTA: start"));
    });
    ArduinoOTA.onEnd([]() {
        Serial.println(F("OTA: end"));
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA: %u%%\r", progress / (total / 100));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA: error %u\n", error);
    });

    ArduinoOTA.begin();
    Serial.println(F("OTA ready"));

    Serial.println(F("Debug: Setup complete"));
}

void loop() {
    ArduinoOTA.handle();

    server.handleClient();

    // Hard end-stop on limit switch (active LOW with INPUT_PULLUP). Latch so
    // further motion into the switch is refused until the user retreats from
    // it or re-homes. AccelStepper::stop() only schedules a decel and would
    // continue driving into the end-stop, so we cancel the target outright.
    if (limitSwitchTriggered()) {
        if (!limitTripped) {
            hardHalt();
            limitTripped = true;
            Serial.println(F("LIMIT triggered: motion latched"));
        }
    }

    stepper.run();
}
