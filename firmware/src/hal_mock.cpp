// Mock sensors for a bare ESP32 (no rig): 4 healthy cells plus faults injected by command.
// Commands (serial console or /api/cmd?c=...):
//   gas K | temp K | sag K | unplug K   (K = 1..4)  inject on cell K
//   room                                         incense in the room (all gas sensors rise)
//   clear                                        remove all injected faults
#if VS_MOCK_SENSORS
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hal.h"

namespace {
float gas_extra[4] = {0, 0, 0, 0};
float temp_extra[4] = {0, 0, 0, 0};
float sag[4] = {0, 0, 0, 0};
bool unplugged[4] = {false, false, false, false};
float room = 0.0f;

float noise(float amp) { return amp * ((float)(esp_random() % 2001) / 1000.0f - 1.0f); }
}  // namespace

bool hal_init(char *report, size_t n) { return hal_selftest(report, n); }

bool hal_selftest(char *report, size_t n) {
    snprintf(report, n, "SELF-TEST PASSED (MOCK: every sensor is simulated, nothing is measured)\n");
    return true;
}

void hal_read(vs_inputs_t &in) {
    float node = 0.0f;
    int n_in = 0;
    for (int k = 0; k < 4; ++k) {
        vs_cell_in_t &c = in.cell[k];
        c.co_v = 0.60f * (1.0f + room + gas_extra[k]) + noise(0.003f);
        c.h2_v = 0.70f * (1.0f + 0.3f * room + gas_extra[k]) + noise(0.003f);
        c.temp_c = 28.0f + temp_extra[k] + noise(0.05f);
        c.temp_ok = !unplugged[k];
        if (unplugged[k]) c.temp_c = -127.0f;
        c.co_ok = c.h2_ok = c.tap_ok = 1;
        const float v = 3.70f - sag[k];
        c.tap_v = node + v + noise(0.001f);
        if (g_hal_last_out.seg_relay_on[k]) {
            node = c.tap_v;
            ++n_in;
        }
    }
    const bool live = g_hal_last_out.pack_relay_on && n_in > 0;
    in.pack_current_a = (live && g_hal_last_out.load_on) ? node / 10.0f : 0.0f;
    in.pack_bus_v = live ? node - in.pack_current_a * 0.2f : 0.0f;
    in.ambient_c = 27.0f + noise(0.05f);
    in.ambient_rh = 55.0f;
    in.current_ok = in.bus_ok = in.ambient_ok = 1;
}

void hal_leds(const vs_outputs_t &, bool) {}
void hal_ui_update(const vs_inputs_t &, const vs_outputs_t &, bool) {}
float hal_tc_read() { return 28.0f + noise(0.25f); }
bool hal_log_begin() { return false; }
void hal_log(const char *) {}
void hal_log_flush() {}
void hal_rtc_set(uint32_t) {}
bool hal_sd_ready() { return false; }
void hal_timestamp(char *buf, size_t n) { snprintf(buf, n, "uptime %lu s", (unsigned long)(millis() / 1000)); }

void hal_command(const char *cmd, char *reply, size_t n) {
    int k = 0;
    reply[0] = 0;
    if (sscanf(cmd, "gas %d", &k) == 1 && k >= 1 && k <= 4) gas_extra[k - 1] = 0.6f;
    else if (sscanf(cmd, "temp %d", &k) == 1 && k >= 1 && k <= 4) temp_extra[k - 1] = 25.0f;
    else if (sscanf(cmd, "sag %d", &k) == 1 && k >= 1 && k <= 4) sag[k - 1] = 0.25f;
    else if (sscanf(cmd, "unplug %d", &k) == 1 && k >= 1 && k <= 4) unplugged[k - 1] = true;
    else if (strcmp(cmd, "room") == 0) room = 0.6f;
    else if (strcmp(cmd, "clear") == 0) {
        for (int i = 0; i < 4; ++i) {
            gas_extra[i] = temp_extra[i] = sag[i] = 0.0f;
            unplugged[i] = false;
        }
        room = 0.0f;
    } else {
        return;
    }
    snprintf(reply, n, "mock: %s", cmd);
}
#endif
