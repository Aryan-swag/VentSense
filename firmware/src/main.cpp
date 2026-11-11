// VentSense controller firmware (ESP32-DevKitC-32E, Arduino framework on FreeRTOS).
//
// Task layout (firmware/FEATURES.md section 0):
//   safety task  - core 1, high priority, 10 Hz: sensors -> safety core -> relays/LEDs, plus the power-up
//                  relay check, the internal-resistance test, test modes and the health estimate.
//                  Task watchdog: a hang reboots the chip; GPIOs reset to high-Z and every relay releases.
//   comms task   - core 0 (with the Wi-Fi stack): dashboard + JSON API (firmware/API.md), serial console,
//                  OLED, microSD log and incident files, network time, phone alerts, firmware update.
//                  It reads snapshots and posts requests; it can never reset a Level 2/3 latch.
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <time.h>

#include "config_table.h"
#include "hal.h"
#include "health.h"
#include "pins.h"
#include "safety_core.h"
#include "sequences.h"
#include "web_index.h"

#ifndef VS_AP_SSID
#define VS_AP_SSID "VentSense"
#endif
#ifndef VS_AP_PASS
#define VS_AP_PASS "ventsense-demo"  // CHANGE before any demo (min 8 chars)
#endif
#ifndef VS_ADMIN_PASS
#define VS_ADMIN_PASS "ventsense-admin"  // CHANGE with the console command: admin <new>
#endif
#define VS_FW "1.1"
#define TZ_OFFSET_S 19800  // IST, UTC+5:30

namespace {

// app notes (event type 100); detail = code
enum { NOTE = 100 };
enum { N_RC_PASS = 1, N_RC_FAIL = 2, N_IR_DONE = 3, N_IR_ABORT = 4, N_CFG = 5, N_IR_BASE = 6, N_SIM = 7, N_OTA = 8 };

// ------------------------------------------------------------------ shared state
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

struct Snapshot {
    vs_inputs_t in;
    vs_outputs_t out;
    vs_health_out_t health;
    vs_irtest_out_t irt;
    vs_rlycheck_out_t rc;
    float ir_now[VS_NCELL], ir_base[VS_NCELL];
    uint8_t armed, sim, profile, ramp;
};
Snapshot g_snap;

vs_config_t g_cfg_active, g_cfg_pending;
volatile bool g_cfg_dirty = false;
volatile bool g_selftest_ok = false, g_operator_armed = true;
volatile bool g_req_load = true, g_req_heater = false, g_req_ack = false, g_req_irtest = false, g_req_rc = false;
volatile bool g_req_ir_baseline = false;
volatile int g_profile = 0;
struct Ramp {
    bool active;
    float rate, target, t0, temp0;
};
volatile bool g_ramp_req = false;
Ramp g_ramp_new = {false, 0, 0, 0, 0};
struct Sim {
    int kind;  // 0 none, 1 gas, 2 temperature
    int cell;
    float until;
};
Sim g_sim = {0, 0, -1};
char g_report[600];
QueueHandle_t g_events;
Preferences prefs;

float now_s() { return (float)(esp_timer_get_time() / 1000) / 1000.0f; }

void get_snapshot(Snapshot &s) {
    portENTER_CRITICAL(&g_mux);
    s = g_snap;
    portEXIT_CRITICAL(&g_mux);
}

void post_note(float t, int code, int cell, int detail) {
    vs_event_t ev = {t, NOTE, cell, code * 1000 + detail};
    xQueueSend(g_events, &ev, 0);
}

// ------------------------------------------------------------------ settings in flash
void load_settings(vs_config_t &cfg, float ir_base[VS_NCELL]) {
    vs_default_config(&cfg);
    prefs.begin("vsapp", true);
    if (prefs.getUInt("cfgsz", 0) == sizeof(vs_config_t)) prefs.getBytes("cfg", &cfg, sizeof cfg);
    for (int k = 0; k < VS_NCELL; ++k) {
        char key[8];
        snprintf(key, sizeof key, "irb%d", k);
        ir_base[k] = prefs.getFloat(key, 0.0f);
        cfg.ir_ohm[k] = ir_base[k];  // the commissioning baseline drives sag compensation
    }
    prefs.end();
}

void save_cfg(const vs_config_t &cfg) {
    prefs.begin("vsapp", false);
    prefs.putBytes("cfg", &cfg, sizeof cfg);
    prefs.putUInt("cfgsz", sizeof(vs_config_t));
    prefs.end();
}

String pref_str(const char *key, const char *def = "") {
    prefs.begin("vsapp", true);
    String v = prefs.getString(key, def);
    prefs.end();
    return v;
}

void pref_put(const char *key, const String &v) {
    prefs.begin("vsapp", false);
    prefs.putString(key, v);
    prefs.end();
}

// ------------------------------------------------------------------ safety task
void safety_task(void *) {
    esp_task_wdt_add(nullptr);
    void *core = vs_create(&g_cfg_active);
    void *health = vs_health_create(nullptr);
    void *irt = vs_irtest_create(nullptr);
    void *rc = vs_rlycheck_create(nullptr);
    vs_config_t cfg = g_cfg_active;
    float ir_base[VS_NCELL], ir_now[VS_NCELL] = {0, 0, 0, 0};
    for (int k = 0; k < VS_NCELL; ++k) ir_base[k] = cfg.ir_ohm[k];
    vs_inputs_t in = {};
    vs_outputs_t out = {};
    vs_health_in_t hin = {};
    vs_health_out_t hout = {};
    vs_irtest_in_t iin = {};
    vs_irtest_out_t iout = {};
    vs_rlycheck_in_t rin = {};
    vs_rlycheck_out_t rout = {};
    Ramp ramp = {false, 0, 0, 0, 0};
    uint8_t prev_rc_state = VS_RC_IDLE, prev_irt_state = VS_IRT_IDLE;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        hal_read(in);
        const float t = now_s();
        in.t_s = t;
        // settings changed from the dashboard: apply between two steps
        if (g_cfg_dirty) {
            portENTER_CRITICAL(&g_mux);
            cfg = g_cfg_pending;
            g_cfg_dirty = false;
            portEXIT_CRITICAL(&g_mux);
            for (int k = 0; k < VS_NCELL; ++k) cfg.ir_ohm[k] = ir_base[k];
            vs_set_config(core, &cfg);
        }
        if (g_req_ir_baseline) {
            g_req_ir_baseline = false;
            bool any = false;
            for (int k = 0; k < VS_NCELL; ++k) any = any || ir_now[k] > 0.0f;
            if (any) {
                Preferences sp;  // own instance: the comms task uses the shared one
                sp.begin("vsapp", false);
                for (int k = 0; k < VS_NCELL; ++k) {
                    ir_base[k] = ir_now[k];
                    cfg.ir_ohm[k] = ir_base[k];
                    char key[8];
                    snprintf(key, sizeof key, "irb%d", k);
                    sp.putFloat(key, ir_base[k]);
                }
                sp.end();
                vs_set_config(core, &cfg);
                post_note(t, N_IR_BASE, -1, 0);
            }
        }
        // demo injection (P2.5c): virtual signal, flagged and logged; a resulting trip is real
        Sim sim;
        portENTER_CRITICAL(&g_mux);
        sim = g_sim;
        portEXIT_CRITICAL(&g_mux);
        const bool sim_on = sim.kind && t < sim.until && sim.cell >= 0 && sim.cell < VS_NCELL;
        if (sim_on) {
            vs_cell_in_t &c = in.cell[sim.cell];
            if (sim.kind == 1) {
                c.co_v *= 1.8f;
                c.h2_v *= 1.8f;
            } else {
                c.temp_c += 25.0f;
            }
        }
        // power-up relay check (P1.8): owns the relays while it runs; arming waits for PASS
        for (int k = 0; k < VS_NCELL; ++k) {
            rin.tap_v[k] = in.cell[k].tap_v;
            rin.tap_ok[k] = in.cell[k].tap_ok;
            rin.segment_enabled[k] = cfg.segment_enabled[k] ? 1 : 0;
        }
        rin.t_s = t;
        rin.bus_v = in.pack_bus_v;
        rin.bus_ok = in.bus_ok;
        rin.start = g_req_rc ? 1 : 0;
        if (g_req_rc) g_req_rc = false;
        vs_rlycheck_step(rc, &rin, &rout);
        if (rout.state != prev_rc_state && (rout.state == VS_RC_PASS || rout.state == VS_RC_FAIL))
            post_note(t, rout.state == VS_RC_PASS ? N_RC_PASS : N_RC_FAIL, rout.fail_cell, rout.fail_code);
        prev_rc_state = rout.state;
        const bool rc_running = rout.active && out.level < 2;
        const bool armed = g_selftest_ok && g_operator_armed && rout.state == VS_RC_PASS;
        in.armed = armed ? 1 : 0;
        if (rout.active) in.bus_ok = 0;  // the core did not command these relays: skip its bus check
        // internal-resistance test (P2.1)
        for (int k = 0; k < VS_NCELL; ++k) {
            iin.cell_v[k] = isnan(out.cell_v[k]) ? 0.0f : out.cell_v[k];
            iin.in_string[k] = out.in_string[k];
        }
        iin.t_s = t;
        iin.pack_current_a = in.pack_current_a;
        iin.current_ok = in.current_ok;
        iin.level = out.level;
        iin.load_on_now = out.load_on;
        iin.start = g_req_irtest ? 1 : 0;
        if (g_req_irtest) g_req_irtest = false;
        vs_irtest_step(irt, &iin, &iout);
        if (iout.result_new) {
            for (int k = 0; k < VS_NCELL; ++k) ir_now[k] = isnan(iout.r_ohm[k]) ? 0.0f : iout.r_ohm[k];
            post_note(t, N_IR_DONE, -1, 0);
        }
        if (iout.state == VS_IRT_ABORTED && prev_irt_state != VS_IRT_ABORTED) post_note(t, N_IR_ABORT, -1, iout.abort_reason);
        prev_irt_state = iout.state;
        // load and heater requests: IR test > load profile > operator
        const int prof = vs_load_profile(g_profile, t);
        in.load_request = iout.load_override >= 0 ? iout.load_override : (prof >= 0 ? prof : (g_req_load ? 1 : 0));
        if (g_ramp_req) {
            portENTER_CRITICAL(&g_mux);
            ramp = g_ramp_new;
            g_ramp_req = false;
            portEXIT_CRITICAL(&g_mux);
            if (ramp.active) {
                ramp.t0 = t;
                ramp.temp0 = in.cell[cfg.heater_cell].temp_c;
            }
        }
        if (ramp.active)
            in.heater_request = (uint8_t)vs_heater_ramp(t, ramp.t0, ramp.temp0, ramp.rate, ramp.target,
                                                        in.cell[cfg.heater_cell].temp_c, 0.25f, 10.0f);
        else in.heater_request = g_req_heater ? 1 : 0;
        in.reset_pressed = hal_reset_button() ? 1 : 0;  // physical button only
        in.ack_pressed = (hal_ack_button() || g_req_ack) ? 1 : 0;
        vs_step(core, &in, &out);
        if (g_req_ack && !hal_ack_button()) g_req_ack = false;
        // health estimate (advisory)
        hin.t_s = t;
        for (int k = 0; k < VS_NCELL; ++k) {
            hin.cell_v[k] = out.cell_v[k];
            hin.in_string[k] = out.in_string[k];
            hin.temp_ok[k] = in.cell[k].temp_ok;
            hin.temp_c[k] = in.cell[k].temp_c;
            hin.gas_base_co[k] = out.gas_base_co[k];
            hin.gas_base_h2[k] = out.gas_base_h2[k];
            hin.ir_now[k] = ir_now[k];
            hin.ir_base[k] = ir_base[k];
        }
        hin.pack_current_a = in.pack_current_a;
        hin.pack_bus_v = in.pack_bus_v;
        hin.current_ok = in.current_ok;
        hin.gas_warm = t >= cfg.gas_warmup_s;
        vs_health_step(health, &hin, &hout);
        // outputs: the relay check drives relays directly while it runs (load and heater off)
        if (rc_running) {
            vs_outputs_t o2 = out;
            for (int k = 0; k < VS_NCELL; ++k) o2.seg_relay_on[k] = rout.seg_on[k];
            o2.pack_relay_on = rout.pack_on;
            o2.load_on = 0;
            o2.heater_on = 0;
            hal_write(o2, t);
            hal_leds(o2, false);
        } else {
            hal_write(out, t);
            hal_leds(out, armed);
        }
        vs_event_t ev;
        while (vs_pop_event(core, &ev)) xQueueSend(g_events, &ev, 0);
        portENTER_CRITICAL(&g_mux);
        g_snap.in = in;
        g_snap.out = out;
        g_snap.health = hout;
        g_snap.irt = iout;
        g_snap.rc = rout;
        for (int k = 0; k < VS_NCELL; ++k) {
            g_snap.ir_now[k] = ir_now[k];
            g_snap.ir_base[k] = ir_base[k];
        }
        g_snap.armed = armed;
        g_snap.sim = sim_on;
        g_snap.profile = (uint8_t)g_profile;
        g_snap.ramp = ramp.active;
        portEXIT_CRITICAL(&g_mux);
        esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
    }
}

// ------------------------------------------------------------------ comms: history, events, incidents
struct Row {
    float t;
    uint8_t lvl, rel;
    float v[VS_NCELL], T[VS_NCELL], g[VS_NCELL];
    float i, bus;
};
const int NHIST = 600;
Row g_hist[NHIST];
int g_hist_n = 0, g_hist_head = 0;  // ring

struct Ev {
    uint32_t id;
    float t;
    int16_t type;
    int8_t cell;
    int32_t detail;
    char ts[24];
};
const int NEV = 120;
Ev g_ev[NEV];
int g_ev_n = 0, g_ev_head = 0;
uint32_t g_ev_id = 0;

struct Incident {
    bool valid;
    char ts[24];
    int cell, mask, level;
};
Incident g_last_inc = {false, "", 0, 0, 0};
File g_inc_file;
float g_inc_post_until = -1.0f;
String g_admin;
String g_ntfy_topic, g_alert_msg;
bool g_time_synced = false, g_reboot = false, g_ota_ok = false;
uint32_t g_alert_last_ms = 0;
float g_tc = NAN;
WebServer server(80);

const Row &hist_at(int i) { return g_hist[(g_hist_head - g_hist_n + i + NHIST) % NHIST]; }

void hist_push(const Snapshot &s) {
    Row &r = g_hist[g_hist_head];
    r.t = s.in.t_s;
    r.lvl = s.out.level;
    r.rel = (uint8_t)(s.out.pack_relay_on << 4);
    for (int k = 0; k < VS_NCELL; ++k) {
        r.rel |= (uint8_t)(s.out.seg_relay_on[k] << k);
        r.v[k] = s.out.cell_v[k];
        r.T[k] = s.in.cell[k].temp_ok ? s.in.cell[k].temp_c : NAN;
        r.g[k] = fmaxf(s.out.gas_rise_co[k], s.out.gas_rise_h2[k]);
    }
    r.i = s.in.pack_current_a;
    r.bus = s.in.pack_bus_v;
    g_hist_head = (g_hist_head + 1) % NHIST;
    if (g_hist_n < NHIST) ++g_hist_n;
}

// "null" for NaN so the JSON stays valid
void jnum(char *b, size_t n, float v, int d) {
    if (isnan(v) || isinf(v)) snprintf(b, n, "null");
    else snprintf(b, n, "%.*f", d, (double)v);
}

int row_csv(const Row &r, char *b, size_t n, char sep_open, char sep_close) {
    char x[16];
    int o = snprintf(b, n, "%c%.1f,%d,%d", sep_open, (double)r.t, r.lvl, r.rel);
    const float *arr[3] = {r.v, r.T, r.g};
    const int dig[3] = {4, 2, 4};
    for (int a = 0; a < 3; ++a)
        for (int k = 0; k < VS_NCELL; ++k) {
            jnum(x, sizeof x, arr[a][k], dig[a]);
            o += snprintf(b + o, n - o, ",%s", x);
        }
    jnum(x, sizeof x, r.i, 4);
    o += snprintf(b + o, n - o, ",%s", x);
    jnum(x, sizeof x, r.bus, 3);
    o += snprintf(b + o, n - o, ",%s%c", x, sep_close);
    return o;
}

String note_text(int detail, int cell) {
    const int code = detail / 1000, d = detail % 1000;
    static const char *rc[] = {"", "output live with every cell bypassed", "cell missing / tap out of range",
                               "relay not switching or pack path open (E-stop?)", "all-on voltage mismatch", "no bus voltage"};
    static const char *ir[] = {"", "alarm active", "relays changed", "load step changed no current", "no current reading",
                               "no cell in string"};
    switch (code) {
        case N_RC_PASS: return "Relay check passed: system can arm";
        case N_RC_FAIL: return String("Relay check FAILED: ") + (d < 6 ? rc[d] : "?") + (cell >= 0 ? " (cell " + String(cell + 1) + ")" : "");
        case N_IR_DONE: return "Internal-resistance test done";
        case N_IR_ABORT: return String("Internal-resistance test aborted: ") + (d < 6 ? ir[d] : "?");
        case N_CFG: return "Settings changed";
        case N_IR_BASE: return "IR baseline saved";
        case N_SIM: return "SIMULATED signal started (demo injection)";
        case N_OTA: return "Firmware update started";
    }
    return "note";
}

void incident_begin(const vs_event_t &ev, const char *ts) {
    g_last_inc.valid = true;
    snprintf(g_last_inc.ts, sizeof g_last_inc.ts, "%s", ts);
    g_last_inc.cell = ev.cell < 0 ? 0 : ev.cell;
    g_last_inc.mask = ev.type == VS_EV_L2_TRIP ? ev.detail : 0;
    g_last_inc.level = ev.type == VS_EV_L2_TRIP ? 2 : 3;
    if (!hal_sd_ready() || g_inc_file) return;
    prefs.begin("vsapp", false);
    const uint32_t n = prefs.getUInt("incn", 0) + 1;
    prefs.putUInt("incn", n);
    prefs.end();
    char name[24];
    snprintf(name, sizeof name, "/inc_%03u.csv", (unsigned)n);
    g_inc_file = SD.open(name, FILE_WRITE);
    if (!g_inc_file) return;
    g_inc_file.printf("# VentSense incident %u, %s, Level %d, cell %d, signal mask %d (1 gas, 2 temperature, 4 voltage)\n",
                      (unsigned)n, ts, g_last_inc.level, g_last_inc.cell + 1, g_last_inc.mask);
    g_inc_file.println("# 10 min before + 2 min after, 1 Hz");
    g_inc_file.println("t,lvl,rel,v1,v2,v3,v4,T1,T2,T3,T4,g1,g2,g3,g4,i,bus");
    char line[200];
    for (int i = 0; i < g_hist_n; ++i) {
        row_csv(hist_at(i), line, sizeof line, ' ', ' ');
        g_inc_file.println(line + 1);
    }
    g_inc_post_until = ev.t_s + 120.0f;
}

void handle_event(const vs_event_t &ev) {
    char ts[24], row[120];
    hal_timestamp(ts, sizeof ts);
    Ev &e = g_ev[g_ev_head];
    e.id = ++g_ev_id;
    e.t = ev.t_s;
    e.type = (int16_t)ev.type;
    e.cell = (int8_t)ev.cell;
    e.detail = ev.detail;
    snprintf(e.ts, sizeof e.ts, "%s", ts);
    g_ev_head = (g_ev_head + 1) % NEV;
    if (g_ev_n < NEV) ++g_ev_n;
    snprintf(row, sizeof row, "E,%s,%.1f,%d,%d,%d", ts, (double)ev.t_s, (int)ev.type, (int)ev.cell, (int)ev.detail);
    Serial.println(row);
    hal_log(row);
    if (ev.type == VS_EV_L2_TRIP || ev.type == VS_EV_L3_TRIP) {
        incident_begin(ev, ts);
        g_alert_msg = ev.type == VS_EV_L2_TRIP ? "VentSense: cell " + String(ev.cell + 1) + " isolated (Level 2) at " + ts
                                               : String("VentSense: pack relay OPEN (Level 3) at ") + ts;
    } else if (ev.type == VS_EV_FAULT_ON) {
        g_alert_msg = "VentSense: sensor fault on cell " + String(ev.cell + 1) + " at " + ts;
    }
}

// ------------------------------------------------------------------ comms: HTTP API (firmware/API.md)
bool is_admin() {
    const String k = server.hasHeader("X-VS-Key") ? server.header("X-VS-Key") : server.arg("key");
    return k.length() && k == g_admin;
}

bool require_admin() {
    if (is_admin()) return true;
    server.send(403, "text/plain", "admin password required");
    return false;
}

void handle_state() {
    Snapshot s;
    get_snapshot(s);
    char ts[24], a[16], b[16], c[16], d[16], e2[16], f2[16];
    hal_timestamp(ts, sizeof ts);
    String j;
    j.reserve(2600);
    char buf[420];
    snprintf(buf, sizeof buf,
             "{\"fw\":\"%s\",\"mock\":%d,\"t\":%.1f,\"time\":\"%s\",\"level\":%d,\"armed\":%d,\"selftest\":%d,"
             "\"relaycheck\":{\"state\":%d,\"phase\":%d,\"code\":%d,\"cell\":%d,\"meas\":%.3f,\"exp\":%.3f},"
             "\"sim\":%d,\"alerts\":%lu,\"buzzer\":%d,",
             VS_FW, (int)VS_MOCK_SENSORS, (double)s.in.t_s, ts, s.out.level, s.armed, g_selftest_ok ? 1 : 0, s.rc.state,
             s.rc.phase, s.rc.fail_code, s.rc.fail_cell, (double)s.rc.measured_v, (double)s.rc.expected_v, s.sim,
             (unsigned long)s.out.alerts, s.out.buzzer_mode);
    j += buf;
    jnum(a, sizeof a, s.in.pack_bus_v, 3);
    jnum(b, sizeof b, s.in.pack_current_a, 4);
    jnum(c, sizeof c, s.in.pack_bus_v * s.in.pack_current_a, 2);
    jnum(d, sizeof d, s.health.pack_ah, 4);
    jnum(e2, sizeof e2, s.health.pack_wh, 3);
    snprintf(buf, sizeof buf, "\"pack\":{\"relay\":%d,\"v\":%s,\"i\":%s,\"w\":%s,\"ah\":%s,\"wh\":%s,\"latched\":%d},",
             s.out.pack_relay_on, a, b, c, d, e2, s.out.pack_latched);
    j += buf;
    jnum(a, sizeof a, s.in.ambient_c, 2);
    jnum(b, sizeof b, s.in.ambient_rh, 1);
    jnum(c, sizeof c, g_tc, 2);
    snprintf(buf, sizeof buf,
             "\"amb\":{\"t\":%s,\"rh\":%s,\"ok\":%d},\"tc\":%s,\"heater\":{\"on\":%d,\"req\":%d,\"duty\":%.3f,\"mode\":\"%s\"},"
             "\"load\":{\"on\":%d,\"req\":%d,\"profile\":%d},\"irtest\":{\"state\":%d,\"reason\":%d,\"di\":%.3f},\"cells\":[",
             a, b, s.in.ambient_ok, c, s.out.heater_on, s.in.heater_request, (double)s.out.heater_duty,
             s.ramp ? "ramp" : (s.in.heater_request ? "on" : "off"), s.out.load_on, s.in.load_request, s.profile, s.irt.state,
             s.irt.abort_reason, (double)s.irt.di_a);
    j += buf;
    for (int k = 0; k < VS_NCELL; ++k) {
        jnum(a, sizeof a, isnan(s.out.cell_v[k]) ? 0.0f : s.out.cell_v[k], 4);
        jnum(b, sizeof b, s.in.cell[k].temp_ok ? s.in.cell[k].temp_c : NAN, 2);
        jnum(c, sizeof c, s.health.soc[k], 3);
        jnum(d, sizeof d, s.health.score[k], 0);
        jnum(e2, sizeof e2, s.out.trip_time_s[k], 1);
        jnum(f2, sizeof f2, s.health.ah_out[k], 4);
        snprintf(buf, sizeof buf,
                 "%s{\"v\":%s,\"t\":%s,\"co\":%.3f,\"h2\":%.3f,\"relay\":%d,\"level\":%d,\"latched\":%d,\"faults\":%d,"
                 "\"sig\":[%d,%d,%d],\"trip_t\":%s,\"order\":[%d,%d,%d],\"soc\":%s,\"ah\":%s,\"score\":%s,\"ir\":%.5f,"
                 "\"ir_base\":%.5f,\"en\":%d}",
                 k ? "," : "", a, b, (double)s.out.gas_rise_co[k], (double)s.out.gas_rise_h2[k], s.out.seg_relay_on[k],
                 s.out.cell_level[k], s.out.cell_latched[k], s.out.cell_faults[k], s.out.sig_active[k][0],
                 s.out.sig_active[k][1], s.out.sig_active[k][2], e2, s.out.trip_order[k][0], s.out.trip_order[k][1],
                 s.out.trip_order[k][2], c, f2, d, (double)s.ir_now[k], (double)s.ir_base[k],
                 g_cfg_active.segment_enabled[k] ? 1 : 0);
        j += buf;
    }
    j += "]}";
    server.send(200, "application/json", j);
}

void handle_history() {
    const float since = server.hasArg("since") ? server.arg("since").toFloat() : -1.0f;
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("{\"cols\":[\"t\",\"lvl\",\"rel\",\"v1\",\"v2\",\"v3\",\"v4\",\"T1\",\"T2\",\"T3\",\"T4\",\"g1\",\"g2\","
                       "\"g3\",\"g4\",\"i\",\"bus\"],\"rows\":[");
    String chunk;
    chunk.reserve(2400);
    char line[200];
    bool first = true;
    for (int i = 0; i < g_hist_n; ++i) {
        const Row &r = hist_at(i);
        if (r.t <= since) continue;
        if (!first) chunk += ",";
        first = false;
        row_csv(r, line, sizeof line, '[', ']');
        chunk += line;
        if (chunk.length() > 2000) {
            server.sendContent(chunk);
            chunk = "";
        }
    }
    chunk += "]}";
    server.sendContent(chunk);
    server.sendContent("");
}

void handle_events() {
    const uint32_t since = server.hasArg("since") ? (uint32_t)server.arg("since").toInt() : 0;
    String j = "{\"next\":" + String(g_ev_id) + ",\"events\":[";
    bool first = true;
    for (int i = 0; i < g_ev_n; ++i) {
        const Ev &e = g_ev[(g_ev_head - g_ev_n + i + NEV) % NEV];
        if (e.id <= since) continue;
        char buf[260];
        snprintf(buf, sizeof buf, "%s{\"id\":%lu,\"t\":%.1f,\"time\":\"%s\",\"type\":%d,\"cell\":%d,\"detail\":%ld",
                 first ? "" : ",", (unsigned long)e.id, (double)e.t, e.ts, e.type, e.cell, (long)e.detail);
        j += buf;
        if (e.type == NOTE) j += ",\"text\":\"" + note_text(e.detail, e.cell) + "\"";
        j += "}";
        first = false;
    }
    j += "]}";
    server.send(200, "application/json", j);
}

void handle_incidents() {
    String j = String("{\"sd\":") + (hal_sd_ready() ? "1" : "0") + ",\"last\":";
    if (g_last_inc.valid) {
        char buf[120];
        snprintf(buf, sizeof buf, "{\"time\":\"%s\",\"cell\":%d,\"mask\":%d,\"level\":%d}", g_last_inc.ts, g_last_inc.cell,
                 g_last_inc.mask, g_last_inc.level);
        j += buf;
    } else {
        j += "null";
    }
    j += ",\"items\":[";
    if (hal_sd_ready()) {
        File root = SD.open("/");
        bool first = true;
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
            const String n = f.name();
            if (n.startsWith("inc_") || n.startsWith("/inc_") || n.startsWith("vs_") || n.startsWith("/vs_")) {
                j += (first ? "" : ",") + String("{\"name\":\"") + (n.startsWith("/") ? n.substring(1) : n) +
                     "\",\"size\":" + String((unsigned long)f.size()) + "}";
                first = false;
            }
            f.close();
        }
    }
    j += "]}";
    server.send(200, "application/json", j);
}

void handle_file() {
    const String n = server.arg("name");
    bool ok = n.length() > 0 && n.length() < 32;
    for (size_t i = 0; ok && i < n.length(); ++i) {
        const char ch = n[i];
        ok = isalnum((unsigned char)ch) || ch == '_' || ch == '.' || ch == '-';  // no paths
    }
    if (!ok || !hal_sd_ready()) {
        server.send(404, "text/plain", "not found");
        return;
    }
    File f = SD.open("/" + n);
    if (!f) {
        server.send(404, "text/plain", "not found");
        return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=" + n);
    server.streamFile(f, "text/csv");
    f.close();
}

void handle_config_get() {
    static char buf[4800];
    const size_t n = vs_config_json(&g_cfg_active, buf, sizeof buf);
    String j = "{\"settings\":";
    j += (n < sizeof buf) ? buf : "{}";
    j += "}";
    server.send(200, "application/json", j);
}

void handle_config_post() {
    if (!require_admin()) return;
    vs_config_t c = g_cfg_active;
    char err[120] = "";
    for (int i = 0; i < server.args(); ++i) {
        const String k = server.argName(i);
        if (k == "key" || k == "plain") continue;
        if (!vs_config_set(&c, k.c_str(), server.arg(i).toFloat(), err, sizeof err)) {
            server.send(200, "application/json", String("{\"ok\":0,\"err\":\"") + err + "\"}");
            return;
        }
    }
    save_cfg(c);
    portENTER_CRITICAL(&g_mux);
    g_cfg_pending = c;
    g_cfg_dirty = true;
    portEXIT_CRITICAL(&g_mux);
    g_cfg_active = c;
    post_note(now_s(), N_CFG, -1, 0);
    server.send(200, "application/json", "{\"ok\":1,\"err\":\"\"}");
}

String command(const String &cin) {
    String c = cin;
    c.trim();
    char reply[700];
    reply[0] = 0;
    int k = 0;
    float r = 0, tg = 0;
    if (c == "arm") {
        g_operator_armed = true;
        Snapshot s;
        get_snapshot(s);
        if (s.rc.state != VS_RC_PASS) g_req_rc = true;
    } else if (c == "disarm") g_operator_armed = false;
    else if (c == "ack") g_req_ack = true;
    else if (c == "relaycheck") g_req_rc = true;
    else if (c == "selftest") {
        g_selftest_ok = hal_selftest(g_report, sizeof g_report);
        return String(g_report);
    } else if (c == "irtest") g_req_irtest = true;
    else if (c == "ir baseline") g_req_ir_baseline = true;
    else if (c == "load on") {
        g_profile = 0;
        g_req_load = true;
    } else if (c == "load off") {
        g_profile = 0;
        g_req_load = false;
    } else if (sscanf(c.c_str(), "profile %d", &k) == 1 && (k == 0 || k == 1)) g_profile = k;
    else if (c == "heater on" || c == "heater off") {
        portENTER_CRITICAL(&g_mux);
        g_ramp_new.active = false;
        g_ramp_req = true;
        portEXIT_CRITICAL(&g_mux);
        g_req_heater = c == "heater on";
    } else if (sscanf(c.c_str(), "ramp %f %f", &r, &tg) == 2) {
        const float cap = g_cfg_active.heater_cutoff_c - 2.0f;
        if (r <= 0.0f || r > 10.0f || tg > cap) return "ramp: rate 0..10 C/min, target <= " + String(cap, 1) + " C";
        portENTER_CRITICAL(&g_mux);
        g_ramp_new = {true, r, tg, 0, 0};
        g_ramp_req = true;
        portEXIT_CRITICAL(&g_mux);
    } else if (c == "sim off") {
        portENTER_CRITICAL(&g_mux);
        g_sim.kind = 0;
        portEXIT_CRITICAL(&g_mux);
    } else if ((sscanf(c.c_str(), "sim gas %d", &k) == 1 || sscanf(c.c_str(), "sim temp %d", &k) == 1) && k >= 1 && k <= 4) {
        portENTER_CRITICAL(&g_mux);
        g_sim = {c.startsWith("sim gas") ? 1 : 2, k - 1, now_s() + 120.0f};
        portEXIT_CRITICAL(&g_mux);
        post_note(now_s(), N_SIM, k - 1, 0);
    } else if (c.startsWith("wifi ")) {
        if (c == "wifi off") {
            pref_put("ssid", "");
            return "ok: Wi-Fi client off after reboot";
        }
        const int sp = c.indexOf(' ', 5);
        if (sp < 0) return "usage: wifi <ssid> <password>";
        pref_put("ssid", c.substring(5, sp));
        pref_put("wpass", c.substring(sp + 1));
        WiFi.begin(c.substring(5, sp).c_str(), c.substring(sp + 1).c_str());
        return "ok: joining " + c.substring(5, sp);
    } else if (c.startsWith("ntfy ")) {
        g_ntfy_topic = c == "ntfy off" ? "" : c.substring(5);
        pref_put("ntfy", g_ntfy_topic);
        return g_ntfy_topic.length() ? "ok: alerts to ntfy.sh/" + g_ntfy_topic : "ok: phone alerts off";
    } else if (c.startsWith("admin ")) {
        const String np = c.substring(6);
        if (np.length() < 8) return "admin password must be at least 8 characters";
        g_admin = np;
        pref_put("admin", g_admin);
    } else {
        hal_command(c.c_str(), reply, sizeof reply);  // real: probes, calibration; mock: simulated sensor faults
        return reply[0] ? String(reply) : "unknown command: " + c;
    }
    return "ok: " + c;
}

void handle_cmd() {
    if (!require_admin()) return;
    server.send(200, "text/plain", command(server.arg("c")));
}

void handle_selftest() {
    g_selftest_ok = hal_selftest(g_report, sizeof g_report);
    String rep = g_report;
    rep.replace("\\", "\\\\");
    rep.replace("\"", "'");
    rep.replace("\n", "\\n");
    server.send(200, "application/json", String("{\"ok\":") + (g_selftest_ok ? "1" : "0") + ",\"report\":\"" + rep + "\"}");
}

bool update_allowed() {
    Snapshot s;
    get_snapshot(s);
    bool latched = s.out.pack_latched;
    for (int k = 0; k < VS_NCELL; ++k) latched = latched || s.out.cell_latched[k];
    return !latched && s.out.level < 2;
}

void handle_update_upload() {
    HTTPUpload &up = server.upload();
    static bool allowed = false;
    if (up.status == UPLOAD_FILE_START) {
        allowed = is_admin() && update_allowed();
        g_ota_ok = false;
        if (allowed) {
            post_note(now_s(), N_OTA, -1, 0);
            allowed = Update.begin(UPDATE_SIZE_UNKNOWN);
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (allowed && Update.write(up.buf, up.currentSize) != up.currentSize) allowed = false;
    } else if (up.status == UPLOAD_FILE_END) {
        g_ota_ok = allowed && Update.end(true);
    }
}

void handle_update_done() {
    if (!is_admin()) {
        server.send(403, "text/plain", "admin password required");
        return;
    }
    if (!g_ota_ok) {
        server.send(200, "text/plain", update_allowed() ? "update failed" : "update refused: a cell is latched or Level >= 2");
        return;
    }
    server.send(200, "text/plain", "update OK: rebooting (all relays release during the reboot)");
    g_reboot = true;
}

void send_alert() {
    // phone alert via ntfy.sh (P2.4). No personal data in the message; TLS without certificate pinning (prototype).
    if (!g_alert_msg.length() || !g_ntfy_topic.length() || WiFi.status() != WL_CONNECTED) return;
    if (millis() - g_alert_last_ms < 10000) return;
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    if (http.begin(tls, "https://ntfy.sh/" + g_ntfy_topic)) {
        http.addHeader("Title", "VentSense alert");
        http.setTimeout(4000);
        http.POST(g_alert_msg);
        http.end();
    }
    g_alert_msg = "";
    g_alert_last_ms = millis();
}

void comms_task(void *) {
    g_admin = pref_str("admin", VS_ADMIN_PASS);
    g_ntfy_topic = pref_str("ntfy");
    const String ssid = pref_str("ssid"), wpass = pref_str("wpass");
    WiFi.mode(ssid.length() ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(VS_AP_SSID, VS_AP_PASS);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);  // lower TX peaks on the AMS1117 3.3 V rail (FEATURES.md section 4)
    if (ssid.length()) WiFi.begin(ssid.c_str(), wpass.c_str());
    const char *hdrs[] = {"X-VS-Key"};
    server.collectHeaders(hdrs, 1);
    server.on("/", [] {
        server.sendHeader("Content-Encoding", "gzip");
        server.send_P(200, "text/html", (const char *)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
    });
    server.on("/api/state", handle_state);
    server.on("/api/history", handle_history);
    server.on("/api/events", handle_events);
    server.on("/api/incidents", handle_incidents);
    server.on("/api/file", handle_file);
    server.on("/api/config", HTTP_GET, handle_config_get);
    server.on("/api/config", HTTP_POST, handle_config_post);
    server.on("/api/cmd", handle_cmd);
    server.on("/api/selftest", handle_selftest);
    server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
    server.onNotFound([] { server.send(404, "text/plain", "not found"); });
    server.begin();
    const bool sd = hal_log_begin();
    Serial.printf("AP %s at %s, microSD %s\n", VS_AP_SSID, WiFi.softAPIP().toString().c_str(), sd ? "on" : "off");
    hal_log("# VentSense log: D=data row, E=event row");
    String line;
    uint32_t next_s = 0, next_flush = 0;
    for (;;) {
        server.handleClient();
        while (Serial.available()) {
            const char ch = (char)Serial.read();
            if (ch == '\n' || ch == '\r') {
                line.trim();
                if (line.length()) Serial.println(command(line));
                line = "";
            } else {
                line += ch;
            }
        }
        vs_event_t ev;
        while (xQueueReceive(g_events, &ev, 0) == pdTRUE) handle_event(ev);
        const uint32_t now = millis();
        g_tc = hal_tc_read();
        if (now >= next_s) {  // once a second: history, log, incident tail, OLED
            next_s = now + 1000;
            Snapshot s;
            get_snapshot(s);
            hist_push(s);
            char ts[24], row[400];
            hal_timestamp(ts, sizeof ts);
            int o = snprintf(row, sizeof row, "D,%s", ts);
            o += row_csv(hist_at(g_hist_n - 1), row + o, sizeof row - o, ',', ' ');
            hal_log(row);
            if (g_inc_file) {
                char l2[200];
                row_csv(hist_at(g_hist_n - 1), l2, sizeof l2, ' ', ' ');
                g_inc_file.println(l2 + 1);
                if (s.in.t_s >= g_inc_post_until) g_inc_file.close();
            }
            hal_ui_update(s.in, s.out, s.armed);
            if (!g_time_synced && WiFi.status() == WL_CONNECTED) {  // network time -> DS3231 (P2.7)
                static bool asked = false;
                if (!asked) {
                    configTime(0, 0, "pool.ntp.org", "time.google.com");
                    asked = true;
                }
                const time_t utc = time(nullptr);
                if (utc > 1700000000) {
                    hal_rtc_set((uint32_t)utc + TZ_OFFSET_S);
                    g_time_synced = true;
                }
            }
        }
        send_alert();
        if (now >= next_flush) {
            hal_log_flush();
            if (g_inc_file) g_inc_file.flush();
            next_flush = now + 5000;
        }
        if (g_reboot) {
            delay(500);
            ESP.restart();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

void setup() {
    hal_gpio_init();  // first: every output LOW (relays released, heater and load off)
    Serial.begin(115200);
    float ir_base[VS_NCELL];
    load_settings(g_cfg_active, ir_base);
    g_cfg_pending = g_cfg_active;
    g_events = xQueueCreate(64, sizeof(vs_event_t));
    g_selftest_ok = hal_init(g_report, sizeof g_report);
    Serial.printf("VentSense firmware %s, mock sensors=%d\n%s", VS_FW, (int)VS_MOCK_SENSORS, g_report);
    if (g_selftest_ok) g_req_rc = true;  // power-up relay check, then the system arms

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wc = {};
    wc.timeout_ms = 2000;
    wc.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wc) != ESP_OK) esp_task_wdt_init(&wc);
#else
    esp_task_wdt_init(2, true);  // 2 s, panic -> reboot -> relays released
#endif

    xTaskCreatePinnedToCore(safety_task, "safety", 8192, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
    xTaskCreatePinnedToCore(comms_task, "comms", 16384, nullptr, 2, nullptr, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
