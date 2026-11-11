// VentSense timed sequences: see sequences.h.
#include "sequences.h"

#include <math.h>
#include <new>
#include <string.h>

namespace {

struct IrTest {
    explicit IrTest(const vs_irtest_config_t &c) : cfg(c) {
        for (int k = 0; k < VS_NCELL; ++k) r[k] = NAN;
    }
    vs_irtest_config_t cfg;
    uint8_t state = VS_IRT_IDLE, reason = VS_IRT_OK, prev_start = 0;
    float t_phase = 0.0f;
    uint8_t load_a = 0, members[VS_NCELL] = {0, 0, 0, 0};
    float sum_v[VS_NCELL] = {0, 0, 0, 0}, sum_i = 0.0f;
    int n = 0;
    float va[VS_NCELL] = {0, 0, 0, 0}, ia = 0.0f;
    float r[VS_NCELL];
    float di = 0.0f;

    void clear_acc() {
        for (int k = 0; k < VS_NCELL; ++k) sum_v[k] = 0.0f;
        sum_i = 0.0f;
        n = 0;
    }
    void abort(uint8_t why) {
        state = VS_IRT_ABORTED;
        reason = why;
    }
    void step(const vs_irtest_in_t &in, vs_irtest_out_t &out) {
        out.result_new = 0;
        const bool start_edge = in.start && !prev_start;
        prev_start = in.start;
        if (start_edge && (state == VS_IRT_IDLE || state == VS_IRT_DONE || state == VS_IRT_ABORTED)) {
            int cnt = 0;
            for (int k = 0; k < VS_NCELL; ++k) {
                members[k] = in.in_string[k];
                cnt += members[k];
            }
            reason = VS_IRT_OK;
            if (in.level != 0) abort(VS_IRT_ABORT_LEVEL);
            else if (!in.current_ok) abort(VS_IRT_ABORT_CURRENT);
            else if (cnt < 1) abort(VS_IRT_ABORT_FEW);
            else {
                state = VS_IRT_PRE;
                t_phase = in.t_s;
                load_a = in.load_on_now;
                clear_acc();
            }
        }
        if (state == VS_IRT_PRE || state == VS_IRT_STEP) {
            bool same = true;
            for (int k = 0; k < VS_NCELL; ++k) same = same && (in.in_string[k] == members[k]);
            if (in.level != 0) abort(VS_IRT_ABORT_LEVEL);
            else if (!same) abort(VS_IRT_ABORT_RELAYS);
            else if (!in.current_ok) abort(VS_IRT_ABORT_CURRENT);
        }
        const float dur = state == VS_IRT_PRE ? cfg.pre_s : cfg.step_s;
        if ((state == VS_IRT_PRE || state == VS_IRT_STEP) && in.t_s >= t_phase + dur - cfg.avg_s) {
            for (int k = 0; k < VS_NCELL; ++k)
                if (members[k]) sum_v[k] += in.cell_v[k];
            sum_i += in.pack_current_a;
            ++n;
        }
        if (state == VS_IRT_PRE && in.t_s >= t_phase + cfg.pre_s && n > 0) {
            for (int k = 0; k < VS_NCELL; ++k) va[k] = sum_v[k] / n;
            ia = sum_i / n;
            clear_acc();
            state = VS_IRT_STEP;
            t_phase = in.t_s;
        } else if (state == VS_IRT_STEP && in.t_s >= t_phase + cfg.step_s && n > 0) {
            float vb[VS_NCELL];
            for (int k = 0; k < VS_NCELL; ++k) vb[k] = sum_v[k] / n;
            const float ib = sum_i / n;
            // rest = the lower-current state
            const bool a_is_rest = ia < ib;
            float res[VS_NCELL];
            di = fabsf(ib - ia);
            if (vs_estimate_ir(a_is_rest ? va : vb, a_is_rest ? vb : va, a_is_rest ? ia : ib, a_is_rest ? ib : ia,
                               cfg.min_di_a, res)) {
                for (int k = 0; k < VS_NCELL; ++k) r[k] = members[k] ? res[k] : NAN;
                state = VS_IRT_DONE;
                out.result_new = 1;
            } else {
                abort(VS_IRT_ABORT_NO_DI);
            }
        }
        if (state == VS_IRT_PRE) out.load_override = (int8_t)load_a;
        else if (state == VS_IRT_STEP) out.load_override = (int8_t)(load_a ? 0 : 1);
        else out.load_override = -1;
        out.state = state;
        out.abort_reason = reason;
        out.di_a = di;
        for (int k = 0; k < VS_NCELL; ++k) out.r_ohm[k] = r[k];
    }
};

struct RlyCheck {
    explicit RlyCheck(const vs_rlycheck_config_t &c) : cfg(c) {}
    vs_rlycheck_config_t cfg;
    uint8_t state = VS_RC_IDLE, prev_start = 0, code = VS_RC_OK;
    int8_t phase = -1, fail_cell = -1;
    float t_phase = 0.0f, meas = 0.0f, expct = 0.0f;
    uint8_t en[VS_NCELL] = {1, 1, 1, 1};

    int next_enabled(int from) const {
        for (int k = from; k < VS_NCELL; ++k)
            if (en[k]) return k;
        return VS_NCELL;  // = the "all on" phase
    }
    void fail(uint8_t c, int8_t cell, float m, float e) {
        state = VS_RC_FAIL;
        code = c;
        fail_cell = cell;
        meas = m;
        expct = e;
    }
    void step(const vs_rlycheck_in_t &in, vs_rlycheck_out_t &out) {
        const bool start_edge = in.start && !prev_start;
        prev_start = in.start;
        if (start_edge && state != VS_RC_RUNNING) {
            state = VS_RC_RUNNING;
            code = VS_RC_OK;
            fail_cell = -1;
            phase = -1;
            t_phase = in.t_s;
            for (int k = 0; k < VS_NCELL; ++k) en[k] = in.segment_enabled[k];
        }
        if (state == VS_RC_RUNNING && in.t_s - t_phase >= cfg.dwell_s) {
            if (!in.bus_ok) {
                fail(VS_RC_NO_BUS, -1, 0.0f, 0.0f);
            } else if (phase == -1) {
                if (fabsf(in.bus_v) > cfg.zero_tol_v) fail(VS_RC_LIVE_WITH_ALL_BYPASSED, -1, in.bus_v, 0.0f);
                else {
                    phase = (int8_t)next_enabled(0);
                    t_phase = in.t_s;
                }
            } else if (phase < VS_NCELL) {
                const int k = phase;
                const float tap = in.tap_v[k];  // all cells below are bypassed, so tap k = cell k voltage
                if (!in.tap_ok[k] || tap < cfg.min_cell_v || tap > cfg.max_cell_v) fail(VS_RC_CELL_MISSING, (int8_t)k, tap, 0.0f);
                else if (fabsf(in.bus_v - tap) > cfg.tol_v) fail(VS_RC_RELAY_NO_SWITCH, (int8_t)k, in.bus_v, tap);
                else {
                    phase = (int8_t)next_enabled(k + 1);
                    t_phase = in.t_s;
                }
            } else {  // all enabled relays on: bus = tap of the highest enabled cell
                float expected = 0.0f;
                for (int k = 0; k < VS_NCELL; ++k)
                    if (en[k]) expected = in.tap_v[k];
                if (fabsf(in.bus_v - expected) > cfg.tol_v) fail(VS_RC_ALL_ON_MISMATCH, -1, in.bus_v, expected);
                else state = VS_RC_PASS;
            }
        }
        out.active = state == VS_RC_RUNNING;
        for (int k = 0; k < VS_NCELL; ++k) {
            uint8_t on = 0;
            if (out.active) on = (phase == k) || (phase == VS_NCELL && en[k]);
            out.seg_on[k] = on;
        }
        out.pack_on = out.active;
        out.state = state;
        out.phase = phase;
        out.fail_code = code;
        out.fail_cell = fail_cell;
        out.measured_v = meas;
        out.expected_v = expct;
    }
};

}  // namespace

extern "C" {

size_t vs_sizeof_irtest_in(void) { return sizeof(vs_irtest_in_t); }
size_t vs_sizeof_irtest_out(void) { return sizeof(vs_irtest_out_t); }

void vs_irtest_default_config(vs_irtest_config_t *c) {
    c->pre_s = 3.0f;
    c->step_s = 1.0f;   // DCIR(1 s)
    c->avg_s = 0.35f;   // ~3-4 samples at 10 Hz
    c->min_di_a = 0.5f; // the 10 ohm load gives ~1.1-1.7 A
}

void *vs_irtest_create(const vs_irtest_config_t *c) {
    vs_irtest_config_t d;
    if (!c) {
        vs_irtest_default_config(&d);
        c = &d;
    }
    return new (std::nothrow) IrTest(*c);
}
void vs_irtest_destroy(void *h) { delete static_cast<IrTest *>(h); }
void vs_irtest_step(void *h, const vs_irtest_in_t *in, vs_irtest_out_t *out) { static_cast<IrTest *>(h)->step(*in, *out); }

void vs_rlycheck_default_config(vs_rlycheck_config_t *c) {
    c->dwell_s = 1.5f;     // PLACEHOLDER: relay operate ~10 ms + ADC/INA219 filtering
    c->tol_v = 0.30f;      // PLACEHOLDER
    c->zero_tol_v = 0.30f; // PLACEHOLDER
    c->min_cell_v = 2.5f;  // 25R discharge cut-off 2.5 V (datasheet, to verify)
    c->max_cell_v = 4.35f;
}

void *vs_rlycheck_create(const vs_rlycheck_config_t *c) {
    vs_rlycheck_config_t d;
    if (!c) {
        vs_rlycheck_default_config(&d);
        c = &d;
    }
    return new (std::nothrow) RlyCheck(*c);
}
void vs_rlycheck_destroy(void *h) { delete static_cast<RlyCheck *>(h); }
void vs_rlycheck_step(void *h, const vs_rlycheck_in_t *in, vs_rlycheck_out_t *out) {
    static_cast<RlyCheck *>(h)->step(*in, *out);
}

int vs_heater_ramp(float t, float t0, float temp0_c, float rate_c_per_min, float target_c, float temp_c, float kp,
                   float window_s) {
    float sp = temp0_c + rate_c_per_min * (t - t0) / 60.0f;
    if (sp > target_c) sp = target_c;
    float duty = kp * (sp - temp_c);
    duty = duty < 0.0f ? 0.0f : (duty > 1.0f ? 1.0f : duty);
    const float phase = fmodf(t - t0, window_s) / window_s;
    return phase < duty ? 1 : 0;
}

int vs_load_profile(int profile, float t) {
    if (profile != 1) return -1;
    // scooter-like: ride 20 s, stop 5 s, ride 10 s, stop 10 s, ride 30 s, stop 5 s (80 s cycle)
    const float c = fmodf(t, 80.0f);
    return (c < 20.0f) || (c >= 25.0f && c < 35.0f) || (c >= 45.0f && c < 75.0f) ? 1 : 0;
}

}  // extern "C"
