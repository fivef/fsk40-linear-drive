# Linear Guide Controller — Project Context

## Overview

PlatformIO firmware for a NodeMCU v3 (ESP8266) that drives a stepper-motor linear guide. Exposes a WiFi web UI and plain HTTP endpoints for motion control with latched limit-switch end-stop protection.

## Hardware

- **MCU:** NodeMCU v3 (ESP-12E, ESP8266)
- **Stepper driver:** A4988 / DRV8825 / TMC22xx (STEP/DIR/EN interface)
- **Motor:** NEMA 17 (or similar) coupled to a lead screw or belt
- **End-stop:** Mechanical/opto switch at min-end of travel (position 0), active LOW with `INPUT_PULLUP`

## Wiring

| NodeMCU | GPIO | Driver    |
|---------|------|-----------|
| D1      | 5    | STEP      |
| D2      | 4    | DIR       |
| D3      | 0    | ENABLE    |
| D5      | 14   | LIMIT sw  |

## Software Architecture

- Single translation unit (`src/main.cpp`), Arduino framework
- `AccelStepper` in DRIVER mode (STEP/DIR), enable pin **not** handed to the library — controlled directly via `digitalWrite` to avoid races
- `ESP8266WebServer` on port 80
- WiFi connect with 20 s timeout; boot continues without network
- ArduinoOTA for over-the-air firmware updates

### Limit-switch Behavior

- Debounced read every loop iteration (two consecutive LOW reads ~0.5 ms apart)
- On trip: `hardHalt()` cancels motion immediately (zeroes target and speed)
- `limitTripped` flag latches:
  - `/move` → rejects targets that don't retreat from switch (409)
  - `/jog` → rejects non-positive deltas; requires physical release (409)
  - `/home` → clears latch and zeroes position

## HTTP API

| Method | Path                                    | Description              |
|--------|-----------------------------------------|--------------------------|
| GET    | `/`                                     | Web UI (auto-refresh)    |
| GET    | `/status`                               | Plain-text status        |
| GET    | `/enable`                               | Power driver on          |
| GET    | `/disable`                              | Decel + power off        |
| GET    | `/stop`                                 | Decel to stop            |
| GET    | `/home`                                 | Zero pos, clear latch    |
| GET    | `/move?pos=<mm>`                        | Absolute move            |
| GET    | `/jog?d=<mm>`                           | Relative jog             |
| GET    | `/profile?speed=<mm/s>&accel=<mm/s²>`   | Set speed/accel          |

## Motion Configuration (`src/main.cpp`)

| Constant              | Default | Notes                        |
|-----------------------|---------|------------------------------|
| `STEPS_PER_MM`        | 40      | e.g. 400 steps/rev ÷ 10 mm   |
| `MAX_TRAVEL_MM`       | 1000    | Soft limit                   |
| `DEFAULT_SPEED_MM_S`  | 20      | mm/s                         |
| `DEFAULT_ACCEL_MM_S2` | 50      | mm/s²                        |
| `ENABLE_ACTIVE`       | HIGH    | Driver polarity              |

## Key Files

| File                  | Purpose                                |
|-----------------------|----------------------------------------|
| `src/main.cpp`        | All firmware logic (323 lines)         |
| `platformio.ini`      | Board, framework, deps, build flags    |
| `secrets.ini`         | WiFi credentials (untracked)           |
| `.gitignore`          | Excludes `.pio/`, `secrets.ini`, etc.  |
| `context.md`          | This file — project overview           |

## Build & Flash

```bash
pio run                        # compile
pio run -t upload              # flash via USB
pio run -t upload --upload-port <ip>  # flash via OTA
pio device monitor             # serial @ 115200 baud
```

## Dependencies

- `waspinator/AccelStepper@^1.64` — step-pulse generation with acceleration
- `bblanchon/ArduinoJson@^7.0.4` — declared but currently unused

## Safety

- Verify `STEPS_PER_MM` and `MAX_TRAVEL_MM` before applying motor power
- HTTP API is **unauthenticated** — run on trusted/isolated network
- `/home` only zeroes position, does **not** perform a homing sequence
- One-sided end-stop (min-end only); max-end switch would require latch logic changes