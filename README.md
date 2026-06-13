# Linear Guide Controller (NodeMCU v3 / ESP8266)

PlatformIO project that drives a stepper-motor-based linear guide from a
NodeMCU v3 (ESP-12E / ESP8266). Exposes a small WiFi web UI plus plain
HTTP endpoints for jogging, absolute moves, homing, motion-profile
control and live status, with a latching limit-switch end-stop.

## Hardware

- **MCU board:** NodeMCU v3 (ESP8266, ESP-12E)
- **Stepper driver:** A4988 / DRV8825 / TMC22xx (STEP / DIR / EN interface)
- **Motor:** NEMA 17 (or similar) coupled to a lead screw or belt linear guide
- **End-stop:** Mechanical / opto switch at the min end of travel (position 0),
  wired to GND, read via `INPUT_PULLUP` (active LOW)

### Wiring

| NodeMCU pin | GPIO   | Connects to                                |
|-------------|--------|--------------------------------------------|
| D1          | GPIO5  | Driver **STEP**                            |
| D2          | GPIO4  | Driver **DIR**                             |
| D3          | GPIO0  | Driver **ENABLE** (active LOW)             |
| D5          | GPIO14 | **LIMIT** switch to GND (INPUT_PULLUP)     |
| GND         | -      | Driver GND, switch GND, common ground      |
| 3V3         | -      | Driver logic VCC if required               |

**Power:** drive the stepper's motor supply (VMOT, typically 12-24 V) from
its own bench/PSU supply sized for the motor. Share grounds between the
NodeMCU and driver. Do **not** power the motor from the NodeMCU 3V3/5V rail.

**GPIO0 caveat:** D3 is a boot-strap pin. The NodeMCU samples it at reset
to choose flash vs. UART-bootloader mode. Most stepper-driver carriers
leave ENABLE high-Z at power-up, which is fine. If your driver actively
pulls ENABLE low at power-up, the ESP8266 will enter the bootloader and
your firmware will not start — in that case move ENABLE to D6/GPIO12 or
D7/GPIO13 in `src/main.cpp`.

## Software architecture

- `src/main.cpp` — entire firmware (single translation unit).
  - WiFi connect with a 20 s timeout (boot continues even without WiFi).
  - `ESP8266WebServer` on port 80.
  - Motion via the [`AccelStepper`](https://github.com/waspinator/AccelStepper)
    library in `DRIVER` (STEP/DIR) mode.
  - The driver **ENABLE** line is owned exclusively by `enableDriver()` via
    direct `digitalWrite`. `AccelStepper::setEnablePin()` is intentionally
    **not** called so the library never toggles the pin behind our back.
- `platformio.ini` — `nodemcuv2` board, Arduino framework, lib deps and
  WiFi credentials passed in as `-D WIFI_SSID="…" -D WIFI_PASSWORD="…"`.

### Limit-switch behavior (latched)

The limit switch is treated as a hard end-stop:

1. The switch is read every loop iteration with a short debounce (two
   consecutive LOW reads ≈ 0.5 ms apart).
2. On trip, `hardHalt()` cancels any pending motion immediately by setting
   the target to the current position and zeroing the speed.
   `AccelStepper::stop()` is **not** used here because it only schedules a
   decel, which would keep driving into the end-stop.
3. The `limitTripped` flag is latched. While latched:
   - `/move` rejects any target that does not strictly retreat from the
     switch (HTTP 409).
   - `/jog` rejects non-positive deltas and requires the switch to physically
     release before a positive jog clears the latch (HTTP 409 otherwise).
   - `/home` clears the latch as part of zeroing the axis.

## Configuration

### WiFi credentials

Credentials live in `secrets.ini` (untracked, excluded by `.gitignore`):

 ```ini
[*]
build_flags =
     -D WIFI_SSID=\"your_ssid\"
     -D WIFI_PASSWORD=\"your_password\"
 ```

`platformio.ini` references it via `extra_configs = secrets.ini`. Create this
file from the template above if you are setting up a fresh checkout.

### Motion parameters

In `src/main.cpp`:

| Constant              | Default | Meaning                                        |
|-----------------------|---------|------------------------------------------------|
| `STEPS_PER_MM`        | 40      | Steps per mm of travel (motor × microstep / lead) |
| `MAX_TRAVEL_MM`       | 1000    | Soft limit at the far end of the guide         |
| `DEFAULT_SPEED_MM_S`  | 20      | Default max speed in mm/s                      |
| `DEFAULT_ACCEL_MM_S2` | 50      | Default acceleration in mm/s²                  |
| `ENABLE_ACTIVE`       | `HIGH`  | Driver ENABLE polarity (most carriers: LOW)    |

Example computation for `STEPS_PER_MM`: a 0.9° motor (400 steps/rev) on a
10 mm/rev lead screw → 400 / 10 = **40 steps/mm**.

### Pin map

Pins are declared as `constexpr uint8_t` at the top of `src/main.cpp`:
`PIN_STEP`, `PIN_DIR`, `PIN_ENABLE`, `PIN_LIMIT`. Edit those if your
wiring differs.

## Build & flash

Install [PlatformIO](https://platformio.org/) (VS Code extension or
standalone CLI), then from the project root.

### First-time flash (USB)

Create `secrets.ini` (see [WiFi credentials](#wifi-credentials)), then
upload over the serial/USB connection:

```bash
pio run -t upload -e nodemcuv2_usb
pio device monitor       # serial monitor @ 115200 baud
```

On boot the firmware prints the assigned IP on the serial monitor (or
reports `WiFi not connected, continuing without network.` after the 20 s
timeout).

### Subsequent flashes (OTA)

Once the device is on the network, upload over the air. The
`platformio.ini` already sets `upload_protocol = espota` and
`upload_port = fsk40-linear-drive.local`, so a plain:

```bash
pio run -t upload
```

…will find the device via mDNS. You can also override the target
explicitly:

```bash
pio run -t upload --upload-port 192.168.1.42
```

## HTTP API

All endpoints respond with `200 ok` on success, `400 missing <arg>` for
malformed requests, and `409 …` when the limit switch is latched against
the requested motion.

| Method | Path                                  | Action                                                          |
|--------|---------------------------------------|-----------------------------------------------------------------|
| GET    | `/`                                   | Web UI (HTML, auto-refresh status every 500 ms)                 |
| GET    | `/status`                             | Plain-text status (see below)                                   |
| GET    | `/enable`                             | Power driver outputs (manual `digitalWrite` on ENABLE)          |
| GET    | `/disable`                            | Decelerate, then power down driver outputs                      |
| GET    | `/stop`                               | Decelerate to a stop (driver remains enabled)                   |
| GET    | `/home`                               | Zero current position and clear limit-switch latch              |
| GET    | `/move?pos=<mm>`                      | Absolute move; clamped to `[0, MAX_TRAVEL_MM]`; auto-enables driver |
| GET    | `/jog?d=<mm>`                         | Relative jog; clamped against soft limits; auto-enables driver  |
| GET    | `/profile?speed=<mm/s>&accel=<mm/s²>` | Update max speed and acceleration                               |

### `/status` output

```
enabled : yes|no
position: <float> mm
target  : <float> mm
running : yes|no
speed   : <float> mm/s
accel   : <float> mm/s^2
limit   : TRIGGERED|clear
latched : yes (move + to clear)|no
```

### Example session

```bash
curl http://<ip>/enable
curl "http://<ip>/profile?speed=30&accel=200"
curl "http://<ip>/move?pos=100"     # absolute move to 100 mm
curl "http://<ip>/jog?d=-5"         # back off 5 mm
curl http://<ip>/status
curl http://<ip>/disable
```

## Safety notes

Read these before powering the motor for the first time.

- **Verify `STEPS_PER_MM` and `MAX_TRAVEL_MM`** before applying motor power.
  A wrong steps/mm value can drive the carriage into a hard stop at full
  configured speed.
- **The HTTP API is unauthenticated.** Anyone on the same network can move
  the axis. Run it on a trusted/isolated network, or add HTTP Basic auth
  (`server.authenticate(...)`) and disable the auto-enable behavior in
  `handleMove` / `handleJog` if exposure is a concern.
- **`/home` does not perform a homing sequence.** It only zeroes the current
  position and clears the latch. If you need repeatable absolute homing,
  replace `handleHome` with a routine that drives toward `PIN_LIMIT` at a
  slow speed until triggered, then zeros there.
- **One-sided end-stop.** The latch logic assumes the limit switch is at
  the min end (position 0). If you fit a switch at the max end as well,
  the latch direction logic in `handleMove` / `handleJog` needs to track
  which end tripped.
- **The driver is auto-enabled** by `/move` and `/jog` if it is not already
  enabled. Use `/disable` explicitly when leaving the machine unattended so
  the motor coils de-energize.

## Repository layout

```
.
├── platformio.ini          # board, framework, lib deps, WiFi build flags
├── src/main.cpp            # firmware (single file)
├── include/                # project headers (currently empty)
├── lib/                    # private libraries (currently empty)
├── .gitignore              # excludes .pio/ build output
└── README.md
```

## Dependencies

Declared in `platformio.ini`:

- `waspinator/AccelStepper@^1.64` — step-pulse generation with acceleration
- `bblanchon/ArduinoJson@^7.0.4` — currently declared but not used; safe to
  remove if you do not plan to add JSON endpoints

## License

No license file is included. Add one (MIT, Apache-2.0, …) if you intend
to share or publish the firmware.
