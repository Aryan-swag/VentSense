// Hardware abstraction: the safety task only talks to these functions.
// Two implementations: src/hal_mock.cpp (VS_MOCK_SENSORS=1) and src/hal_real.cpp (rig).
#pragma once
#include "safety_core.h"

// GPIO outputs to a safe state (all LOW = relays released, heater/load off). Call first.
void hal_gpio_init();
// Last outputs written (used by the mock to model the taps).
extern vs_outputs_t g_hal_last_out;
// One-time init of buses and sensors. Returns true if every sensor answered (self-test, P1.8).
bool hal_init();
// Fill all sensor fields of `in` (not t_s / armed / requests). Called at 10 Hz from the safety task.
void hal_read(vs_inputs_t &in);
// Drive relays, gate-driver inputs and buzzer from the core outputs.
void hal_write(const vs_outputs_t &out, float t_s);
// Buttons (debounced in the HAL). Pressed = true.
bool hal_reset_button();
bool hal_ack_button();
// Mock only: inject a scenario from the serial console / dashboard ("simulate fault", P2.5c).
void hal_mock_command(const char *cmd);
