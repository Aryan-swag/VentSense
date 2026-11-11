// Real sensor drivers for the rig. TODO as the modules arrive (build order step 4):
//   ADS1115 x3 (0x48 CO, 0x49 H2, 0x4B taps; tap volts x6 then per-channel 2-point calibration)
//   DS18B20 x4 on GPIO 4 (start all conversions together, 12-bit, 750 ms; -127 / 85.0 = invalid)
//   INA219 (0x40, pack current + bus voltage), BME280 (0x76), DS3231, SSD1306, PCF8574 x2, MAX6675
// Until then this stub reports every sensor as invalid and the self-test as failed, so the core
// never arms and every relay stays released (fail-secure).
#if !VS_MOCK_SENSORS
#include <Arduino.h>

#include "hal.h"

bool hal_init() { return false; }

void hal_read(vs_inputs_t &in) {
    for (int k = 0; k < 4; ++k) {
        in.cell[k].co_ok = in.cell[k].h2_ok = in.cell[k].temp_ok = in.cell[k].tap_ok = 0;
    }
    in.current_ok = in.bus_ok = in.ambient_ok = 0;
}

void hal_mock_command(const char *) {}
#endif
