#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
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
// EEPROM / WiFi credential storage
// ---------------------------------------------------------------------------
constexpr int EEPROM_SIZE = 128;
constexpr int EEPROM_CRED_FLAG = 0;
constexpr int EEPROM_SSID_ADDR = 1;
constexpr int EEPROM_PASS_ADDR = 33;
constexpr uint8_t CRED_MAGIC = 0xAA;

// ---------------------------------------------------------------------------
// Captive portal (AP mode) configuration
// ---------------------------------------------------------------------------
constexpr int AP_CHANNEL = 1;
constexpr int AP_MAX_CLIENTS = 4;
constexpr unsigned long AP_CONNECT_TIMEOUT_MS = 20000;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);
ESP8266WebServer server(80);
DNSServer dnsServer;

bool driverEnabled = false;
bool apMode = false;

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
// EEPROM credential helpers
// ---------------------------------------------------------------------------
bool readCredentials(String &ssid, String &pass) {
    EEPROM.begin(EEPROM_SIZE);
    if (EEPROM.read(EEPROM_CRED_FLAG) != CRED_MAGIC) {
        EEPROM.end();
        return false;
    }
    char ssidBuf[33];
    for (int i = 0; i < 32; i++) {
        ssidBuf[i] = EEPROM.read(EEPROM_SSID_ADDR + i);
        if (ssidBuf[i] == '\0') break;
    }
    ssidBuf[32] = '\0';
    ssid = String(ssidBuf);
    char passBuf[65];
    for (int i = 0; i < 64; i++) {
        passBuf[i] = EEPROM.read(EEPROM_PASS_ADDR + i);
        if (passBuf[i] == '\0') break;
    }
    passBuf[64] = '\0';
    pass = String(passBuf);
    EEPROM.end();
    return true;
}

void writeCredentials(const String &ssid, const String &pass) {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.write(EEPROM_CRED_FLAG, CRED_MAGIC);
    for (unsigned int i = 0; i < 32 && i < ssid.length(); i++) {
        EEPROM.write(EEPROM_SSID_ADDR + i, ssid[i]);
    }
    for (unsigned int i = ssid.length(); i < 32; i++) {
        EEPROM.write(EEPROM_SSID_ADDR + i, '\0');
    }
    for (unsigned int i = 0; i < 64 && i < pass.length(); i++) {
        EEPROM.write(EEPROM_PASS_ADDR + i, pass[i]);
    }
    for (unsigned int i = pass.length(); i < 64; i++) {
        EEPROM.write(EEPROM_PASS_ADDR + i, '\0');
    }
    EEPROM.commit();
    EEPROM.end();
}

// ---------------------------------------------------------------------------
// Captive portal (AP mode)
// ---------------------------------------------------------------------------
const char CONFIG_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>WiFi Setup — Linear Guide</title>
  <style>
    body{font-family:sans-serif;margin:2em;max-width:480px}
    input{font-size:1em;padding:.4em .6em;margin:.2em;width:100%;box-sizing:border-box}
    button{font-size:1em;padding:.6em 1em;margin:.2em}
    label{display:block;margin-top:.8em;font-weight:bold}
    h1{font-size:1.3em}
    .hint{color:#666;font-size:.9em;margin-top:.3em}
  </style>
</head>
<body>
  <h1>&#9881; Linear Guide WiFi Setup</h1>
  <p>Enter your 2.4&nbsp;GHz WiFi credentials. The device will save them permanently and reboot.</p>
  <form method="POST" action="/save">
    <label for="ssid">Network name (SSID)</label>
    <input id="ssid" name="ssid" placeholder="e.g. MyHomeWiFi" required>
    <label for="pass">Password</label>
    <input id="pass" name="pass" type="password" placeholder="leave empty for open network">
    <p><button type="submit">Save &amp; Connect</button></p>
  </form>
  <div class="hint">Connect to the <strong>LinearGuide-XXXX</strong> network to configure.</div>
</body>
</html>
)HTML";

const char SAVED_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>WiFi Saved — Linear Guide</title>
  <style>
    body{font-family:sans-serif;margin:2em;max-width:480px;text-align:center}
    .ok{color:#090;font-size:2em}
  </style>
</head>
<body>
  <p class="ok">&#10003;</p>
  <h1>Credentials Saved</h1>
  <p>Device is rebooting and will try to connect to your WiFi network.</p>
  <p>Once connected, find it via the serial monitor or your router's DHCP lease list.</p>
</body>
</html>
)HTML";

void startAPMode() {
    apMode = true;

    // Derive AP SSID from the chip's MAC to make it unique.
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apSsid[24];
    snprintf_P(apSsid, sizeof(apSsid), PSTR("LinearGuide-%02X%02X"), mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(apSsid, nullptr, AP_CHANNEL, false, AP_MAX_CLIENTS);

    dnsServer.start(53, "*", WiFi.softAPIP());

    Serial.print(F("AP mode started. SSID: "));
    Serial.println(apSsid);
    Serial.print(F("AP IP: "));
    Serial.println(WiFi.softAPIP());
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
    if (apMode) {
        server.send_P(200, "text/html", CONFIG_HTML);
    } else {
        server.send_P(200, "text/html", INDEX_HTML);
    }
}

void handleConfig() {
    server.send_P(200, "text/html", CONFIG_HTML);
}

void handleSave() {
    if (!server.hasArg("ssid")) {
        server.send(400, "text/plain", "missing ssid");
        return;
    }
    String ssid = server.arg("ssid");
    String pass = server.hasArg("pass") ? server.arg("pass") : "";

    ssid.trim();
    if (ssid.length() == 0) {
        server.send(400, "text/plain", "SSID cannot be empty");
        return;
    }

    writeCredentials(ssid, pass);
    server.send_P(200, "text/html", SAVED_HTML);
    server.close();
    delay(500);

    Serial.printf("Credentials saved. SSID: %s. Rebooting...\n", ssid.c_str());
    ESP.restart();
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
    stepper.setCurrentPosition(0);
    limitTripped = false;
    server.send(200, "text/plain", "ok");
}

void handleMove() {
    if (!server.hasArg("pos")) { server.send(400, "text/plain", "missing pos"); return; }
    float mm = server.arg("pos").toFloat();
    mm = constrain(mm, 0.0f, MAX_TRAVEL_MM);
    long targetSteps = mmToSteps(mm);

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
// WiFi connection helper
// ---------------------------------------------------------------------------
bool tryConnect(const String &ssid, const String &pass, unsigned long timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.hostname("fsk40-linear-drive");
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());

    Serial.printf("Connecting to %s", ssid.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    return WiFi.status() == WL_CONNECTED;
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println(F("Linear guide controller starting..."));
    Serial.println(F("Debug: Firmware version 1.0"));

    pinMode(PIN_ENABLE, OUTPUT);
    enableDriver(false);
    pinMode(PIN_LIMIT, INPUT_PULLUP);

    stepper.setMaxSpeed(DEFAULT_SPEED_MM_S * STEPS_PER_MM);
    stepper.setAcceleration(DEFAULT_ACCEL_MM_S2 * STEPS_PER_MM);
    stepper.setPinsInverted(true, false, false);

    // ---- WiFi connection with fallback to captive portal ----
    bool wifiOk = false;
    String eepromSsid, eepromPass;

    if (readCredentials(eepromSsid, eepromPass)) {
        Serial.println(F("Found stored credentials in EEPROM"));
        wifiOk = tryConnect(eepromSsid, eepromPass, AP_CONNECT_TIMEOUT_MS);
    }

#ifdef WIFI_SSID
    if (!wifiOk) {
        Serial.println(F("Trying compile-time WiFi credentials"));
        wifiOk = tryConnect(WIFI_SSID, WIFI_PASSWORD, AP_CONNECT_TIMEOUT_MS);
    }
#endif

    if (wifiOk) {
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
        Serial.println(F("Starting AP mode with captive portal..."));
        startAPMode();
    }

    // ---- HTTP routes (_always_ registered; handleRoot checks apMode) ----
    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/enable", handleEnable);
    server.on("/disable", handleDisable);
    server.on("/stop", handleStop);
    server.on("/home", handleHome);
    server.on("/move", handleMove);
    server.on("/jog", handleJog);
    server.on("/profile", handleProfile);
    server.on("/config", handleConfig);
    server.on("/save", handleSave);
    server.begin();
    Serial.println(F("HTTP server started"));

    // ---- OTA ----
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

    if (apMode) {
        dnsServer.processNextRequest();
    }

    server.handleClient();

    if (limitSwitchTriggered()) {
        if (!limitTripped) {
            hardHalt();
            limitTripped = true;
            Serial.println(F("LIMIT triggered: motion latched"));
        }
    }

    stepper.run();
}