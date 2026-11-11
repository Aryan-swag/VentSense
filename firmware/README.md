# VentSense controller firmware

ESP32-DevKitC-32E, Arduino framework on FreeRTOS, built with PlatformIO. The feature plan is in [`FEATURES.md`](FEATURES.md).

## Layout

```
lib/safety_core/   hardware-free C/C++ safety core: 2-of-3 voting, latching, smart gas/temperature/voltage
                   signals, sensor faults, relay verification, heater limits. Also compiled by sim/.
include/pins.h     pin map + I2C addresses (design spec 5.4 / 5.5)
include/hal.h      hardware abstraction used by the safety task
src/main.cpp       safety task (core 1, 10 Hz, task watchdog) + comms task (core 0: Wi-Fi AP, web page, JSON API, serial console)
src/hal_common.cpp relays, gate-driver inputs, buzzer patterns, buttons (same for mock and real)
src/hal_mock.cpp   simulated sensors for a bare ESP32 (VS_MOCK_SENSORS=1)
src/hal_real.cpp   real drivers: ADS1115 x3, DS18B20 x4 (non-blocking, probe mapping), INA219, BME280, DS3231, SSD1306,
                   PCF8574 LEDs, MAX6675, microSD log, tap calibration (NVS). Compiles; NOT yet run on hardware
test/              native unit tests for the safety core (Unity)
```

## Commands (PlatformIO CLI: `~/.platformio/penv/bin/pio`, or the VS Code PlatformIO buttons)

| Command | What it does |
|---|---|
| `pio test -e native` | 22 safety-core unit tests on the laptop (passing, 11 Nov 2026) |
| `pio run -e esp32dev_mock` | ESP32 build with simulated sensors (builds: RAM 14.9 %, flash 53.4 %) |
| `pio run -e esp32dev_mock -t upload -t monitor` | flash a bare DevKitC and open the serial console |
| `pio run -e esp32dev` | build for the real rig (builds: RAM 15.7 %, flash 61.6 %; min_spiffs partitions) |

**Mock build on a bare DevKitC:**
- Join the Wi-Fi network `VentSense` (password in `src/main.cpp`; **change it**), then open `http://192.168.4.1`.
- Serial or `/api/cmd?c=` commands:
  - fault injection: `gas 3`, `temp 3`, `sag 2`, `unplug 1`, `room`, `clear`;
  - controls: `heater on|off`, `load on|off`, `ack`, `arm|disarm`.
- **Reset is only the physical button** on GPIO 35. That is safety rule 2.

**Real build console commands** (step-by-step in [`BRINGUP.md`](BRINGUP.md)):

| Command | What it does |
|---|---|
| `selftest` | Re-check every module. Arming needs 3 × ADS1115, the INA219 and 4 mapped DS18B20s |
| `probes`, then `probe K N` | List the DS18B20 probes and assign probe N to cell K (saved to flash) |
| `calpt K <V>` | Two-point tap calibration against a multimeter |
| `calshow`, `calclear K` | Show or reset the calibration |

## Safety design in the code

- **Relays are active-HIGH** (decision 41). GPIOs are written LOW before they become outputs. Boot, crash, a watchdog reboot or a failed self-test all leave every relay released.
- **The safety task never waits on Wi-Fi.** It hands a snapshot to the comms task under a spinlock and pushes events through a queue.
- **Thresholds marked PLACEHOLDER** in `vs_default_config()` are first guesses to tune on the rig. Values marked SPEC come from the design spec.

## Known TODO

- Run the real drivers on the modules (`BRINGUP.md`) and fix whatever the hardware disagrees with (items marked VERIFY).
- P1 features:
  - WebSocket dashboard with charts (LittleFS);
  - incident report on SD (continuous 1 Hz logging is already in place);
  - calibration wizard page and thresholds in NVS;
  - admin password on `/api/cmd`;
  - relay-by-relay self-test sequence.
