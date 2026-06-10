#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
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
//   Example: 1.8deg motor (200 steps/rev) + 1/16 microstepping + 8 mm/rev
//   lead screw  =>  200 * 16 / 8 = 400 steps/mm
constexpr float STEPS_PER_MM = 400.0f;

// Maximum travel of the guide in millimetres (soft limit).
constexpr float MAX_TRAVEL_MM = 300.0f;

// Default motion profile.
constexpr float DEFAULT_SPEED_MM_S = 20.0f;   // mm/s
constexpr float DEFAULT_ACCEL_MM_S2 = 100.0f; // mm/s^2

// Driver enable is typically active LOW.
constexpr uint8_t ENABLE_ACTIVE = LOW;
constexpr uint8_t ENABLE_INACTIVE = HIGH;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);
ESP8266WebServer server(80);

bool driverEnabled = false;

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
    <input id="spd" type="number" step="1" value="20">
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
    server.send(200, "text/plain", s);
}

void handleEnable()  { enableDriver(true);  server.send(200, "text/plain", "ok"); }
void handleDisable() { stepper.stop(); enableDriver(false); server.send(200, "text/plain", "ok"); }
void handleStop()    { stepper.stop(); server.send(200, "text/plain", "ok"); }

void handleHome() {
    // Treat current position as zero. Replace with a real homing routine that
    // drives toward PIN_LIMIT until triggered if you have an end-stop installed.
    stepper.setCurrentPosition(0);
    server.send(200, "text/plain", "ok");
}

void handleMove() {
    if (!server.hasArg("pos")) { server.send(400, "text/plain", "missing pos"); return; }
    float mm = server.arg("pos").toFloat();
    mm = constrain(mm, 0.0f, MAX_TRAVEL_MM);
    if (!driverEnabled) enableDriver(true);
    stepper.moveTo(mmToSteps(mm));
    server.send(200, "text/plain", "ok");
}

void handleJog() {
    if (!server.hasArg("d")) { server.send(400, "text/plain", "missing d"); return; }
    float delta = server.arg("d").toFloat();
    float target = stepsToMm(stepper.currentPosition()) + delta;
    target = constrain(target, 0.0f, MAX_TRAVEL_MM);
    if (!driverEnabled) enableDriver(true);
    stepper.moveTo(mmToSteps(target));
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
    Serial.println(F("Linear guide controller starting..."));

    pinMode(PIN_ENABLE, OUTPUT);
    enableDriver(false);
    pinMode(PIN_LIMIT, INPUT_PULLUP);

    stepper.setMaxSpeed(DEFAULT_SPEED_MM_S * STEPS_PER_MM);
    stepper.setAcceleration(DEFAULT_ACCEL_MM_S2 * STEPS_PER_MM);
    stepper.setEnablePin(PIN_ENABLE);
    stepper.setPinsInverted(false, false, true); // step, dir, enable (enable active LOW)
    stepper.disableOutputs();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to %s", WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("IP: "));
        Serial.println(WiFi.localIP());
    } else {
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
}

void loop() {
    server.handleClient();

    // Soft limit check
    long minSteps = 0;
    long maxSteps = mmToSteps(MAX_TRAVEL_MM);
    long cur = stepper.currentPosition();
    if (cur < minSteps || cur > maxSteps) {
        stepper.stop();
    }

    // Emergency stop on limit switch (active LOW with INPUT_PULLUP)
    if (digitalRead(PIN_LIMIT) == LOW && stepper.isRunning()) {
        stepper.stop();
    }

    stepper.run();
}
