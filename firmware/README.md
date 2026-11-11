# VentSense controller firmware

ESP32-DevKitC-32E, Arduino framework on FreeRTOS, built with PlatformIO.

| Document | What it covers |
|---|---|
| [`FEATURES.md`](FEATURES.md) | Agreed feature plan and its status |
| [`API.md`](API.md) | Dashboard / HTTP API and console commands |
| [`BRINGUP.md`](BRINGUP.md) | Step-by-step first run on the hardware |

## Layout

```
lib/safety_core/src/   hardware-free C/C++ (also compiled into the digital twin in ../sim):
  safety_core.*        2-of-3 voting, latching, smart gas/temperature/voltage signals, sensor faults,
                       relay verification, heater limits, undervoltage load cutoff
  health.*             charge/energy counting, state of charge, health indicator (advisory)
  sequences.*          internal-resistance test, power-up relay check, heater ramp, load profile
  config_table.*       editable settings with hard limits (spec values can only be tightened)
include/pins.h         pin map + I2C addresses (design spec 5.4 / 5.5)
include/hal.h          hardware abstraction
src/main.cpp           safety task (core 1, 10 Hz, watchdog) + comms task (core 0: dashboard, API, console,
                       OLED, SD log + incident files, network time, phone alerts, firmware update)
src/hal_common.cpp     relays, gate-driver inputs, buzzer, buttons
src/hal_real.cpp       real drivers (ADS1115 x3, DS18B20 x4, INA219, BME280, DS3231, SSD1306, PCF8574, MAX6675, SD)
src/hal_mock.cpp       simulated sensors for a bare ESP32 (VS_MOCK_SENSORS=1)
web/index.html         the dashboard (embedded gzipped at build time by tools/embed_web.py)
test/                  native unit tests (Unity)
```

## Commands (PlatformIO CLI `~/.platformio/penv/bin/pio`, or the VS Code PlatformIO buttons)

| Command | Result (11 Nov 2026) |
|---|---|
| `pio test -e native` | 39 unit tests pass (safety core 23, health/sequences/settings 16) |
| `pio run -e esp32dev_mock` | Builds: RAM 30.5 %, flash 65.9 % |
| `pio run -e esp32dev` | Builds: RAM 31.1 %, flash 71.9 % (real drivers; not yet run on hardware) |
| `pio run -e <env> -t upload -t monitor` | Flash and open the serial console |
| `python3 ../sim/dash_server.py` | Run the dashboard on the laptop against the digital twin (`http://localhost:8080`) |

## Dashboard

1. Join Wi-Fi `VentSense` and open `http://192.168.4.1`. The tabs are Live, Trends, Events & incidents, Health, Control and Settings.
2. Read-only views need no password. Commands and settings need the admin password (default `ventsense-admin`). **Change it** with `admin <new>`, and change the Wi-Fi password in `src/main.cpp` (`VS_AP_PASS`).
3. No dashboard action can reset a Level 2/3 latch: that's the physical button on GPIO 35 (safety rule 2).

## Safety design in the code

- **Relays are active-HIGH** (decision 41). GPIOs are driven LOW before they become outputs, so boot, crash, a watchdog reboot, a failed self-test or a failed relay check all leave every relay released.
- **Power-up sequence:** self-test (modules + DS18B20 probe mapping), then the relay check (pack relay, each bypass relay alone, then all, checked against the INA219 bus voltage). Only then does the system arm.
- **The safety task never waits on Wi-Fi.** The comms task gets snapshots and posts requests; settings are applied between two safety steps.
- **The undervoltage load cutoff** switches the load off when any cell falls below `cell_uv_v` (placeholder 3.0 V). The rig has no BMS, and this stops the 10 Ω load from over-discharging the cells.
- **Thresholds marked PLACEHOLDER** in `vs_default_config()` are to be tuned on the rig. Values marked SPEC come from the design spec.

## Known gaps

- Nothing has run on real hardware yet. Follow `BRINGUP.md` and check every VERIFY item.
- **Phone alerts** use TLS without certificate pinning (prototype). Messages carry no personal data.
- **Admin password** travels in a header over the WPA2 access point. Fine for a lab demo; not for deployment.
- **Not started (P3):** ML anomaly detection, health-trend prediction, cloud logging.
