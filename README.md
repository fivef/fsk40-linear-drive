# Linear Guide Controller (NodeMCU v3 / ESP8266)

PlatformIO project that drives a stepper-motor-based linear guide from a
NodeMCU v3 (ESP-12E / ESP8266) board. Provides a small web UI for jogging,
absolute moves, homing and live status.

## Hardware

- **MCU board:** NodeMCU v3 (ESP8266, ESP-12E)
- **Stepper driver:** A4988 / DRV8825 / TMC22xx (STEP / DIR / EN interface)
- **Motor:** NEMA 17 (or similar) coupled to a lead screw / belt linear guide
- **Optional:** End-stop / limit switch wired to ground

### Wiring

| NodeMCU pin | GPIO   | Driver pin           |
|-------------|--------|----------------------|
| D1          | GPIO5  | STEP                 |
| D2          | GPIO4  | DIR                  |
| D3          | GPIO0  | ENABLE (active LOW)  |
| D5          | GPIO14 | LIMIT switch (to GND, INPUT_PULLUP) |
| GND         | -      | Driver GND, switch GND |
| 3V3         | -      | Driver logic VCC (if needed) |

> Power the stepper driver's motor supply (VMOT) from a separate supply
> sized for your motor (typically 12-24 V). Share grounds between the
> NodeMCU and the driver, but do **not** power the motor from the NodeMCU.

## Configuration

Edit `platformio.ini` and set your WiFi credentials:

```ini
build_flags =
    -D WIFI_SSID=\"your_ssid\"
    -D WIFI_PASSWORD=\"your_password\"
```

Tune motion parameters in `src/main.cpp`:

- `STEPS_PER_MM` — steps per millimetre of travel (depends on motor steps/rev,
  microstepping and lead screw / pulley pitch).
- `MAX_TRAVEL_MM` — soft limit for the guide length.
- `DEFAULT_SPEED_MM_S`, `DEFAULT_ACCEL_MM_S2` — default motion profile.

## Build & flash

Install [PlatformIO](https://platformio.org/) (CLI or VS Code extension), then:

```bash
pio run                  # build
pio run -t upload        # flash via USB
pio device monitor       # serial monitor @ 115200
```

## Usage

After flashing, open the serial monitor to find the assigned IP address, then
browse to `http://<ip>/`. Endpoints:

| URL                          | Action                          |
|------------------------------|---------------------------------|
| `/`                          | Web UI                          |
| `/status`                    | Plain-text status               |
| `/enable` / `/disable`       | Enable / disable driver outputs |
| `/stop`                      | Decelerate to a stop            |
| `/home`                      | Set current position as zero    |
| `/move?pos=<mm>`             | Absolute move (clamped to soft limits) |
| `/jog?d=<mm>`                | Relative jog                    |
| `/profile?speed=<mm/s>&accel=<mm/s^2>` | Update motion profile |

## Safety

- The default `home` handler just zeros the current position. Replace it with
  a proper homing routine that drives toward the limit switch if you need
  true repeatable homing.
- Always verify `STEPS_PER_MM` and `MAX_TRAVEL_MM` before powering the motor,
  especially on a guide with hard end-stops.
