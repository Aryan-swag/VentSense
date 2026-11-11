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
src/hal_real.cpp   real sensor drivers: TODO as the modules arrive; until then the self-test fails and every relay stays released
test/              native unit tests for the safety core (Unity)
```

## Commands (PlatformIO CLI: `~/.platformio/penv/bin/pio`, or the VS Code PlatformIO buttons)

| Command | What it does |
|---|---|
| `pio test -e native` | 22 safety-core unit tests on the laptop (passing, 11 Nov 2026) |
| `pio run -e esp32dev_mock` | ESP32 build with simulated sensors (builds: RAM 14.7 %, flash 79.9 %) |
| `pio run -e esp32dev_mock -t upload -t monitor` | flash a bare DevKitC and open the serial console |
| `pio run -e esp32dev` | build for the real rig (sensor drivers still TODO) |

**Mock build on a bare DevKitC:**
- Join the Wi-Fi network `VentSense` (password in `src/main.cpp`; **change it**), then open `http://192.168.4.1`.
- Serial or `/api/cmd?c=` commands:
  - fault injection: `gas 3`, `temp 3`, `sag 2`, `unplug 1`, `room`, `clear`;
  - controls: `heater on|off`, `load on|off`, `ack`, `arm|disarm`.
- **Reset is only the physical button** on GPIO 35. That is safety rule 2.

## Safety design in the code

- **Relays are active-HIGH** (decision 41). GPIOs are written LOW before they become outputs. Boot, crash, a watchdog reboot or a failed self-test all leave every relay released.
- **The safety task never waits on Wi-Fi.** It hands a snapshot to the comms task under a spinlock and pushes events through a queue.
- **Thresholds marked PLACEHOLDER** in `vs_default_config()` are first guesses to tune on the rig. Values marked SPEC come from the design spec.

## Known TODO

- Real drivers: ADS1115 ×3, DS18B20 ×4, INA219, BME280, DS3231, SSD1306, PCF8574 ×2, MAX6675, microSD.
- P1 features: WebSocket dashboard, SD black box, calibration wizard and NVS settings, admin password on `/api/cmd`.
- Flash is at 80 % of the default 1.3 MB app partition, so OTA plus web assets will need a partition change (e.g. `min_spiffs`).
