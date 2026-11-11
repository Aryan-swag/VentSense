// VentSense safety core: see safety_core.h for the contract.
#include "safety_core.h"

#include <math.h>
#include <new>
#include <string.h>

namespace {

const float NEVER = -1.0e9f;
const int QSIZE = 64;

// Rate estimate from two exponential moving averages: for a ramp input the slow EMA
// lags the fast one by rate * (tau_slow - tau_fast). Robust to sample noise.
struct DualEma {
    float fast = 0.0f, slow = 0.0f;
    bool init = false;
    void reset(float v) {
        fast = slow = v;
        init = true;
    }
    // returns units per second
    float update(float v, float dt, float tau) {
        if (!init) {
            reset(v);
            return 0.0f;
        }
        const float tf = tau * 0.25f, ts = tau;
        fast += (dt / (tf + dt)) * (v - fast);
        slow += (dt / (ts + dt)) * (v - slow);
        return (fast - slow) / (ts - tf);
    }
};

struct SigState {
    bool raw = false;
    bool active = false;
    float raw_change_t = NEVER;
    float last_active_t = NEVER;
    float activated_t = NEVER;
};

struct CellState {
    float co_base = 0.0f, h2_base = 0.0f;
    bool base_init = false;
    DualEma co_rate, h2_rate, temp_rate;
    SigState sig[VS_NSIG];
    bool latched = false;
    float trip_t = -1.0f;
    uint8_t order[VS_NSIG] = {255, 255, 255};
    uint8_t faults = 0;
};

float median(const float *v, int n) {
    float a[VS_NCELL];
    for (int i = 0; i < n; ++i) a[i] = v[i];
    for (int i = 1; i < n; ++i)  // insertion sort, n <= 4
        for (int j = i; j > 0 && a[j - 1] > a[j]; --j) {
            float t = a[j];
            a[j] = a[j - 1];
            a[j - 1] = t;
        }
    if (n <= 0) return 0.0f;
    return (n & 1) ? a[n / 2] : 0.5f * (a[n / 2 - 1] + a[n / 2]);
}

class Core {
  public:
    explicit Core(const vs_config_t &c) : cfg(c) {}

    vs_config_t cfg;
    CellState cell[VS_NCELL];
    bool pack_latched = false;
    float pack_trip_t = -1.0f;
    bool first = true;
    float t0 = 0.0f, last_t = 0.0f;
    bool prev_reset = false, prev_ack = false, prev_armed = false;
    uint8_t acked_level = 0;
    uint8_t seg_cmd[VS_NCELL] = {0, 0, 0, 0};
    float seg_change_t[VS_NCELL] = {0, 0, 0, 0};
    uint8_t pack_cmd = 0;
    float pack_change_t = 0.0f;
    uint32_t alerts_prev = 0;
    bool heater_cut = false, duty_block = false;
    uint8_t heater_prev = 0;
    float duty = 0.0f;

    vs_event_t q[QSIZE];
    int qhead = 0, qcount = 0;

    void push(float t, int type, int c, int detail) {
        int idx = (qhead + qcount) % QSIZE;
        if (qcount == QSIZE) {  // full: drop the oldest
            qhead = (qhead + 1) % QSIZE;
            --qcount;
            idx = (qhead + qcount) % QSIZE;
        }
        q[idx].t_s = t;
        q[idx].type = type;
        q[idx].cell = c;
        q[idx].detail = detail;
        ++qcount;
    }
    int pop(vs_event_t *ev) {
        if (qcount == 0) return 0;
        *ev = q[qhead];
        qhead = (qhead + 1) % QSIZE;
        --qcount;
        return 1;
    }

    void step(const vs_inputs_t &in, vs_outputs_t &out);
};

void Core::step(const vs_inputs_t &in, vs_outputs_t &out) {
    const float t = in.t_s;
    float dt = first ? 0.1f : t - last_t;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 5.0f) dt = 5.0f;
    if (first) {
        t0 = t;
        for (int k = 0; k < VS_NCELL; ++k) seg_change_t[k] = t;
        pack_change_t = t;
    }
    const float since_start = t - t0;
    const bool armed = in.armed != 0;
    if (armed != prev_armed || first) {
        if (armed) push(t, VS_EV_ARMED, -1, 0);
        else if (!first) push(t, VS_EV_DISARMED, -1, 0);
    }

    // ------------------------------------------------------------ physical reset (edge)
    const bool reset_edge = in.reset_pressed && !prev_reset;
    if (reset_edge) {
        for (int k = 0; k < VS_NCELL; ++k) {
            CellState &c = cell[k];
            c.latched = false;
            c.trip_t = -1.0f;
            for (int s = 0; s < VS_NSIG; ++s) {
                c.order[s] = 255;
                c.sig[s].last_active_t = NEVER;
            }
        }
        pack_latched = false;
        pack_trip_t = -1.0f;
        acked_level = 0;
        push(t, VS_EV_RESET, -1, 0);
    }

    // ------------------------------------------------------------ cell voltages from the taps
    // string node 0 = pack negative; node k = tap k if relay k is in the string, else node k-1.
    // A bypassed cell's + terminal still reads node(k-1) + its open-circuit voltage.
    float cv[VS_NCELL];
    float node = 0.0f;
    bool node_valid = true;
    for (int k = 0; k < VS_NCELL; ++k) {
        const vs_cell_in_t &ci = in.cell[k];
        cv[k] = (ci.tap_ok && node_valid) ? ci.tap_v - node : NAN;
        if (seg_cmd[k]) {
            if (ci.tap_ok) node = ci.tap_v;
            else node_valid = false;
        }
    }

    // ------------------------------------------------------------ sensor faults
    for (int k = 0; k < VS_NCELL; ++k) {
        const vs_cell_in_t &ci = in.cell[k];
        uint8_t f = 0;
        if (!ci.co_ok || ci.co_v < cfg.gas_fault_low_v || ci.co_v > cfg.gas_fault_high_v) f |= VS_FAULT_CO;
        if (!ci.h2_ok || ci.h2_v < cfg.gas_fault_low_v || ci.h2_v > cfg.gas_fault_high_v) f |= VS_FAULT_H2;
        if (!ci.temp_ok || ci.temp_c < cfg.temp_valid_min_c || ci.temp_c > cfg.temp_valid_max_c) f |= VS_FAULT_TEMP;
        if (!ci.tap_ok) f |= VS_FAULT_TAP;
        const uint8_t changed = f ^ cell[k].faults;
        for (int b = 0; b < 4; ++b) {
            const uint8_t bit = (uint8_t)(1u << b);
            if (changed & bit) push(t, (f & bit) ? VS_EV_FAULT_ON : VS_EV_FAULT_OFF, k, bit);
        }
        cell[k].faults = f;
    }

    // ------------------------------------------------------------ gas
    const bool warm = since_start >= cfg.gas_warmup_s;
    float rise[2][VS_NCELL], rate[2][VS_NCELL];
    bool gvalid[2][VS_NCELL];
    for (int k = 0; k < VS_NCELL; ++k) {
        CellState &c = cell[k];
        const float v[2] = {in.cell[k].co_v, in.cell[k].h2_v};
        float *base[2] = {&c.co_base, &c.h2_base};
        DualEma *re[2] = {&c.co_rate, &c.h2_rate};
        const uint8_t fbit[2] = {VS_FAULT_CO, VS_FAULT_H2};
        for (int s = 0; s < 2; ++s) {
            gvalid[s][k] = !(c.faults & fbit[s]);
            rise[s][k] = 0.0f;
            rate[s][k] = 0.0f;
            if (!gvalid[s][k]) continue;
            if (!c.base_init || !warm) {  // warm-up: follow the sensor, don't vote
                *base[s] = v[s];
                re[s]->reset(0.0f);
                gvalid[s][k] = warm;
                continue;
            }
            const float b = *base[s] > cfg.gas_min_baseline_v ? *base[s] : cfg.gas_min_baseline_v;
            rise[s][k] = (v[s] - *base[s]) / b;
            rate[s][k] = re[s]->update(rise[s][k], dt, cfg.gas_slope_tau_s) * 60.0f;
            // baseline: follow slowly while quiet, follow quickly downwards (drift), freeze while rising
            const float a = dt / (cfg.gas_baseline_tau_s + dt);
            if (v[s] < *base[s]) *base[s] += (10.0f * a > 1.0f ? 1.0f : 10.0f * a) * (v[s] - *base[s]);
            else if (rise[s][k] < 0.5f * cfg.gas_rise_frac && !c.sig[VS_SIG_GAS].raw) *base[s] += a * (v[s] - *base[s]);
        }
        c.base_init = true;
    }
    bool gas_raw[VS_NCELL] = {false, false, false, false};
    bool ambient_gas = false;
    for (int s = 0; s < 2; ++s) {
        int n_high = 0;
        for (int k = 0; k < VS_NCELL; ++k)
            if (gvalid[s][k] && rise[s][k] >= cfg.gas_rise_frac) ++n_high;
        if (n_high >= cfg.gas_ambient_min_cells) ambient_gas = true;
        for (int k = 0; k < VS_NCELL; ++k) {
            if (!gvalid[s][k]) continue;
            bool cand = rise[s][k] >= cfg.gas_rise_frac ||
                        (rate[s][k] >= cfg.gas_rate_frac_per_min && rise[s][k] >= cfg.gas_rate_min_rise_frac);
            if (cand && cfg.gas_cm_reject) {
                float others[VS_NCELL];
                int n = 0;
                for (int j = 0; j < VS_NCELL; ++j)
                    if (j != k && gvalid[s][j]) others[n++] = rise[s][j];
                if (n > 0 && rise[s][k] - median(others, n) < cfg.gas_cm_margin_frac) cand = false;
            }
            if (cand) gas_raw[k] = true;
        }
    }

    // ------------------------------------------------------------ temperature
    bool temp_raw[VS_NCELL] = {false, false, false, false};
    float trate[VS_NCELL] = {0, 0, 0, 0};
    for (int k = 0; k < VS_NCELL; ++k) {
        CellState &c = cell[k];
        if (c.faults & VS_FAULT_TEMP) {
            c.temp_rate.init = false;
            continue;
        }
        const float T = in.cell[k].temp_c;
        trate[k] = c.temp_rate.update(T, dt, cfg.temp_slope_tau_s) * 60.0f;
        float others[VS_NCELL];
        int n = 0;
        for (int j = 0; j < VS_NCELL; ++j)
            if (j != k && !(cell[j].faults & VS_FAULT_TEMP)) others[n++] = in.cell[j].temp_c;
        bool r = T >= cfg.temp_abs_c;
        if (in.ambient_ok && T - in.ambient_c >= cfg.temp_above_ambient_c) r = true;
        if (n > 0 && T - median(others, n) >= cfg.temp_above_neighbors_c) r = true;
        if (trate[k] >= cfg.temp_rate_c_per_min) r = true;
        temp_raw[k] = r;
    }

    // ------------------------------------------------------------ voltage sag + relay verification
    bool in_str[VS_NCELL];
    bool all_settled = true;
    for (int k = 0; k < VS_NCELL; ++k) {
        const bool settled = (t - seg_change_t[k]) >= cfg.relay_settle_s;
        if (!settled) all_settled = false;
        in_str[k] = seg_cmd[k] && cfg.segment_enabled[k] && settled && !isnan(cv[k]) && !(cell[k].faults & VS_FAULT_TAP);
    }
    bool volt_raw[VS_NCELL] = {false, false, false, false};
    float vdev[VS_NCELL] = {0, 0, 0, 0};
    bool mismatch = false;
    for (int k = 0; k < VS_NCELL; ++k) {
        if (!in_str[k]) continue;
        if (cv[k] < cfg.cell_v_min_valid) mismatch = true;
        float others[VS_NCELL], irs[VS_NCELL];
        int n = 0;
        bool ir_known = cfg.ir_ohm[k] > 0.0f;
        for (int j = 0; j < VS_NCELL; ++j)
            if (j != k && in_str[j]) {
                others[n] = cv[j];
                irs[n] = cfg.ir_ohm[j];
                if (cfg.ir_ohm[j] <= 0.0f) ir_known = false;
                ++n;
            }
        if (n == 0) continue;
        float comp = 0.0f;
        if (ir_known && in.current_ok) comp = (cfg.ir_ohm[k] - median(irs, n)) * in.pack_current_a;
        vdev[k] = median(others, n) - cv[k] - comp;
        volt_raw[k] = vdev[k] >= cfg.volt_sag_v;
    }
    if (pack_cmd && all_settled && (t - pack_change_t) >= cfg.relay_settle_s && in.bus_ok && in.current_ok && node_valid) {
        const float expected = node - in.pack_current_a * cfg.path_r_ohm;
        if (fabsf(in.pack_bus_v - expected) > cfg.relay_check_tol_v) mismatch = true;
    }
    if (!pack_cmd && (t - pack_change_t) >= cfg.relay_settle_s && in.bus_ok && in.pack_bus_v > cfg.relay_check_tol_v)
        mismatch = true;  // pack relay commanded open but the output is live

    // ------------------------------------------------------------ debounce
    const bool raw_all[VS_NSIG][VS_NCELL] = {
        {gas_raw[0], gas_raw[1], gas_raw[2], gas_raw[3]},
        {temp_raw[0], temp_raw[1], temp_raw[2], temp_raw[3]},
        {volt_raw[0], volt_raw[1], volt_raw[2], volt_raw[3]}};
    for (int k = 0; k < VS_NCELL; ++k)
        for (int s = 0; s < VS_NSIG; ++s) {
            SigState &g = cell[k].sig[s];
            const bool r = raw_all[s][k];
            if (r != g.raw) {
                g.raw = r;
                g.raw_change_t = t;
            }
            if (g.raw && !g.active && t - g.raw_change_t >= cfg.debounce_s - 1e-4f) {
                g.active = true;
                g.activated_t = t;
                push(t, VS_EV_SIGNAL_ON, k, s);
            } else if (!g.raw && g.active && t - g.raw_change_t >= cfg.debounce_s - 1e-4f) {
                g.active = false;
                push(t, VS_EV_SIGNAL_OFF, k, s);
            }
            if (g.active) g.last_active_t = t;
        }

    // ------------------------------------------------------------ 2-of-3 vote per cell (Level 2)
    for (int k = 0; k < VS_NCELL; ++k) {
        CellState &c = cell[k];
        if (c.latched) continue;
        int n = 0, mask = 0;
        for (int s = 0; s < VS_NSIG; ++s)
            if (c.sig[s].active || (t - c.sig[s].last_active_t) <= cfg.window_s) {
                ++n;
                mask |= 1 << s;
            }
        if (n >= 2) {
            c.latched = true;
            c.trip_t = t;
            int m = 0;  // order of crossing
            bool used[VS_NSIG] = {false, false, false};
            for (int i = 0; i < n; ++i) {
                int best = -1;
                for (int s = 0; s < VS_NSIG; ++s)
                    if ((mask & (1 << s)) && !used[s] && (best < 0 || c.sig[s].activated_t < c.sig[best].activated_t)) best = s;
                used[best] = true;
                c.order[m++] = (uint8_t)best;
            }
            push(t, VS_EV_L2_TRIP, k, mask);
        }
    }

    // ------------------------------------------------------------ Level 3: two or more cells latched
    int nlatched = 0, lmask = 0;
    for (int k = 0; k < VS_NCELL; ++k)
        if (cell[k].latched) {
            ++nlatched;
            lmask |= 1 << k;
        }
    if (!pack_latched && nlatched >= 2) {
        pack_latched = true;
        pack_trip_t = t;
        push(t, VS_EV_L3_TRIP, -1, lmask);
    }

    // ------------------------------------------------------------ relay commands (active-HIGH, fail-secure)
    for (int k = 0; k < VS_NCELL; ++k) {
        const uint8_t want = (armed && cfg.segment_enabled[k] && !cell[k].latched) ? 1 : 0;
        if (want != seg_cmd[k]) {
            seg_cmd[k] = want;
            seg_change_t[k] = t;
        }
    }
    const uint8_t pack_want = (armed && !pack_latched) ? 1 : 0;
    if (pack_want != pack_cmd) {
        pack_cmd = pack_want;
        pack_change_t = t;
    }

    // ------------------------------------------------------------ heater
    const int hc = (cfg.heater_cell >= 0 && cfg.heater_cell < VS_NCELL) ? cfg.heater_cell : 0;
    const bool hvalid = !(cell[hc].faults & VS_FAULT_TEMP);
    const float hT = in.cell[hc].temp_c;
    if (hvalid && hT >= cfg.heater_cutoff_c) heater_cut = true;
    if (heater_cut && hvalid && hT < cfg.heater_rearm_c && !in.heater_request) heater_cut = false;
    duty += (dt / (cfg.heater_duty_tau_s + dt)) * ((float)heater_prev - duty);
    if (duty >= cfg.heater_max_duty) duty_block = true;
    if (duty_block && duty <= cfg.heater_max_duty - 0.05f) duty_block = false;
    const uint8_t heater_on =
        (in.heater_request && hvalid && !heater_cut && !duty_block && nlatched == 0 && !pack_latched) ? 1 : 0;
    heater_prev = heater_on;

    // ------------------------------------------------------------ levels, alerts, buzzer
    uint32_t alerts = 0;
    if (ambient_gas) alerts |= VS_ALERT_AMBIENT_GAS;
    if (mismatch) alerts |= VS_ALERT_RELAY_MISMATCH;
    if (!in.current_ok) alerts |= VS_ALERT_CURRENT_FAULT;
    if (!in.ambient_ok) alerts |= VS_ALERT_AMBIENT_FAULT;
    if (heater_cut) alerts |= VS_ALERT_HEATER_CUTOFF;
    if (duty_block && in.heater_request) alerts |= VS_ALERT_HEATER_DUTY;
    if (!armed) alerts |= VS_ALERT_NOT_ARMED;
    uint8_t level = 0;
    for (int k = 0; k < VS_NCELL; ++k) {
        bool any = cell[k].faults != 0;
        for (int s = 0; s < VS_NSIG; ++s) any = any || cell[k].sig[s].active;
        out.cell_level[k] = cell[k].latched ? 2 : (any ? 1 : 0);
        if (cell[k].faults) alerts |= VS_ALERT_SENSOR_FAULT;
        if (out.cell_level[k] > level) level = out.cell_level[k];
    }
    const uint32_t level1_alerts =
        VS_ALERT_AMBIENT_GAS | VS_ALERT_RELAY_MISMATCH | VS_ALERT_CURRENT_FAULT | VS_ALERT_AMBIENT_FAULT | VS_ALERT_SENSOR_FAULT;
    if (level < 1 && (alerts & level1_alerts)) level = 1;
    if (pack_latched) level = 3;
    const uint32_t ach = alerts ^ alerts_prev;
    for (int b = 0; b < 32; ++b)
        if (ach & (1u << b)) push(t, (alerts & (1u << b)) ? VS_EV_ALERT_ON : VS_EV_ALERT_OFF, -1, (int)(1u << b));
    alerts_prev = alerts;

    const bool ack_edge = in.ack_pressed && !prev_ack;
    if (ack_edge) {
        acked_level = level;
        push(t, VS_EV_ACK, -1, level);
    }
    if (level < acked_level) acked_level = level;

    // ------------------------------------------------------------ outputs
    for (int k = 0; k < VS_NCELL; ++k) {
        out.seg_relay_on[k] = seg_cmd[k];
        out.cell_latched[k] = cell[k].latched;
        out.cell_faults[k] = cell[k].faults;
        for (int s = 0; s < VS_NSIG; ++s) {
            out.sig_active[k][s] = cell[k].sig[s].active;
            out.trip_order[k][s] = cell[k].order[s];
        }
        out.cell_v[k] = cv[k];
        out.gas_rise_co[k] = rise[0][k];
        out.gas_rise_h2[k] = rise[1][k];
        out.temp_rate_c_per_min[k] = trate[k];
        out.volt_dev_v[k] = vdev[k];
        out.trip_time_s[k] = cell[k].trip_t;
    }
    out.pack_relay_on = pack_cmd;
    out.heater_on = heater_on;
    out.load_on = (in.load_request && pack_cmd && !pack_latched) ? 1 : 0;
    out.level = level;
    out.pack_latched = pack_latched;
    out.alerts = alerts;
    out.buzzer_mode = level > acked_level ? level : 0;
    out.pack_trip_time_s = pack_trip_t;
    out.heater_duty = duty;

    prev_reset = in.reset_pressed;
    prev_ack = in.ack_pressed;
    prev_armed = armed;
    last_t = t;
    first = false;
}

}  // namespace

extern "C" {

void vs_default_config(vs_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->window_s = 90.0f;   // SPEC 60-120 s; middle value
    c->debounce_s = 0.5f;  // PLACEHOLDER: 5 samples; keeps "isolation < 1 s after the 2nd signal" (SPEC)
    c->gas_warmup_s = 180.0f;          // PLACEHOLDER: MQ warm-up, check the module datasheets
    c->gas_rise_frac = 0.30f;          // PLACEHOLDER
    c->gas_rate_frac_per_min = 0.20f;  // PLACEHOLDER
    c->gas_rate_min_rise_frac = 0.10f; // PLACEHOLDER
    c->gas_cm_margin_frac = 0.20f;     // PLACEHOLDER
    c->gas_cm_reject = 1;
    c->gas_ambient_min_cells = 3;      // PLACEHOLDER: 3 of 4 cells rising together = room air
    c->gas_baseline_tau_s = 600.0f;    // PLACEHOLDER
    c->gas_min_baseline_v = 0.05f;
    c->gas_fault_low_v = 0.02f;        // PLACEHOLDER: open sensor
    c->gas_fault_high_v = 3.10f;       // 5 V x 22/37 = 2.97 V is the divider maximum (SPEC 5.3)
    c->gas_slope_tau_s = 20.0f;        // PLACEHOLDER
    c->temp_abs_c = 50.0f;             // PLACEHOLDER (25R discharge ambient limit is 60 C, SPEC 5.1)
    c->temp_above_ambient_c = 15.0f;   // PLACEHOLDER
    c->temp_above_neighbors_c = 8.0f;  // PLACEHOLDER
    c->temp_rate_c_per_min = 2.0f;     // PLACEHOLDER
    c->temp_slope_tau_s = 30.0f;       // PLACEHOLDER
    c->temp_valid_min_c = -20.0f;
    c->temp_valid_max_c = 120.0f;      // DS18B20 range is -55..125 C; 85.0 power-on value is handled by the driver
    c->volt_sag_v = 0.10f;             // PLACEHOLDER
    c->cell_v_min_valid = 0.5f;        // PLACEHOLDER
    for (int k = 0; k < VS_NCELL; ++k) {
        c->ir_ohm[k] = 0.0f;
        c->segment_enabled[k] = 1;
    }
    c->relay_settle_s = 0.5f;          // PLACEHOLDER (relay operate time is ms; this also covers ADC filtering)
    c->relay_check_tol_v = 0.30f;      // PLACEHOLDER
    c->path_r_ohm = 0.20f;             // PLACEHOLDER: 0.1 ohm shunt (SPEC) + fuse, relay, wiring
    c->heater_cell = 0;
    c->heater_cutoff_c = 55.0f;        // SPEC 5.1
    c->heater_rearm_c = 50.0f;         // PLACEHOLDER
    c->heater_max_duty = 0.50f;        // SPEC 5.1
    c->heater_duty_tau_s = 60.0f;      // PLACEHOLDER
}

size_t vs_sizeof_config(void) { return sizeof(vs_config_t); }
size_t vs_sizeof_inputs(void) { return sizeof(vs_inputs_t); }
size_t vs_sizeof_outputs(void) { return sizeof(vs_outputs_t); }
size_t vs_sizeof_event(void) { return sizeof(vs_event_t); }

void *vs_create(const vs_config_t *cfg) {
    vs_config_t d;
    if (!cfg) {
        vs_default_config(&d);
        cfg = &d;
    }
    return new (std::nothrow) Core(*cfg);
}

void vs_destroy(void *core) { delete static_cast<Core *>(core); }

void vs_set_config(void *core, const vs_config_t *cfg) { static_cast<Core *>(core)->cfg = *cfg; }

void vs_step(void *core, const vs_inputs_t *in, vs_outputs_t *out) { static_cast<Core *>(core)->step(*in, *out); }

int vs_pop_event(void *core, vs_event_t *ev) { return static_cast<Core *>(core)->pop(ev); }

int vs_estimate_ir(const float v_rest[VS_NCELL], const float v_load[VS_NCELL], float i_rest, float i_load,
                   float min_di_a, float out_r[VS_NCELL]) {
    const float di = i_load - i_rest;
    if (fabsf(di) < min_di_a) return 0;
    for (int k = 0; k < VS_NCELL; ++k) out_r[k] = (v_rest[k] - v_load[k]) / di;
    return 1;
}

}  // extern "C"
