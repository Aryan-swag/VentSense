// Hardware abstraction. Two implementations: src/hal_mock.cpp (VS_MOCK_SENSORS=1, bare ESP32)
// and src/hal_real.cpp (the rig). src/hal_common.cpp holds the GPIO parts shared by both.
//
// Threading: functions marked [safety] are called only from the safety task (core 1);
// [comms] only from the comms task (core 0). The real HAL guards the shared I2C bus with a mutex.
#pragma once
#include <stddef.h>

#include "safety_core.h"

// GPIO outputs to a safe state (all LOW = relays released, heater/load off). Call first.  [setup]
void hal_gpio_init();
// Last outputs written (the mock uses it to model the taps).
extern vs_outputs_t g_hal_last_out;

// Buses, devices, stored calibration; then runs hal_selftest(). True = safe to arm.     [setup]
bool hal_init(char *report, size_t n);
// Re-check every device (power-up self-test, P1.8). Writes a human-readable report.
// True only if everything the safety vote needs is present: 3 x ADS1115, INA219,
// 4 x DS18B20 with a confirmed probe-to-cell mapping.                                    [comms]
bool hal_selftest(char *report, size_t n);

void hal_read(vs_inputs_t &in);                      // all sensor fields of `in`, 10 Hz   [safety]
void hal_write(const vs_outputs_t &out, float t_s);  // relays, gate inputs, buzzer          [safety]
void hal_leds(const vs_outputs_t &out, bool armed);  // 12 status LEDs via PCF8574 (P2.8)   [safety]
bool hal_reset_button();                             // debounced, pressed = true          [safety]
bool hal_ack_button();                               //                                    [safety]

void hal_ui_update(const vs_inputs_t &in, const vs_outputs_t &out, bool armed);  // OLED   [comms]
float hal_tc_read();                    // MAX6675 reference thermocouple, NAN if open/missing [comms]
bool hal_log_begin();                   // microSD; false if no card                          [comms]
void hal_log(const char *line);         // append one CSV line to today's log                 [comms]
void hal_log_flush();                   //                                                    [comms]
void hal_timestamp(char *buf, size_t n);  // "2026-11-12 14:32:05" from the DS3231, else uptime [comms]
// Console / dashboard commands. Mock: fault injection. Real: probe mapping, calibration.  [comms]
void hal_command(const char *cmd, char *reply, size_t n);
