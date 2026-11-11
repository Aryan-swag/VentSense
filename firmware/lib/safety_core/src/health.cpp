// VentSense health estimates: see health.h.
#include "health.h"

#include <math.h>
#include <new>
#include <string.h>

namespace {

float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

float median_of(const float *v, int n) {
    float a[VS_NCELL];
    for (int i = 0; i < n; ++i) a[i] = v[i];
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && a[j - 1] > a[j]; --j) {
            const float t = a[j];
            a[j] = a[j - 1];
            a[j - 1] = t;
        }
    if (n <= 0) return 0.0f;
    return (n & 1) ? a[n / 2] : 0.5f * (a[n / 2 - 1] + a[n / 2]);
}

struct Health {
    explicit Health(const vs_health_config_t &c) : cfg(c) {
        for (int k = 0; k < VS_NCELL; ++k) {
            soc[k] = NAN;
            rest_since[k] = -1.0f;
            gas0_co[k] = gas0_h2[k] = NAN;
        }
    }
    vs_health_config_t cfg;
    bool first = true;
    float last_t = 0.0f;
    float soc[VS_NCELL];
    float rest_since[VS_NCELL];
    float ah[VS_NCELL] = {0, 0, 0, 0};
    float pack_ah = 0.0f, pack_wh = 0.0f;
    float gas0_co[VS_NCELL], gas0_h2[VS_NCELL];

    void step(const vs_health_in_t &in, vs_health_out_t &out) {
        float dt = first ? 0.0f : in.t_s - last_t;
        if (dt < 0.0f || dt > 5.0f) dt = 0.0f;
        const float I = in.current_ok ? in.pack_current_a : 0.0f;
        if (in.current_ok) {
            pack_ah += I * dt / 3600.0f;
            pack_wh += I * in.pack_bus_v * dt / 3600.0f;
        }
        for (int k = 0; k < VS_NCELL; ++k) {
            const bool v_ok = !isnan(in.cell_v[k]) && in.cell_v[k] > 0.5f;
            const float ik = in.in_string[k] ? I : 0.0f;
            // a cell is at rest when it carries (almost) no current: bypassed, or the pack is idle
            const bool resting = in.current_ok && fabsf(ik) < cfg.rest_current_a;
            if (resting) {
                if (rest_since[k] < 0.0f) rest_since[k] = in.t_s;
            } else {
                rest_since[k] = -1.0f;
            }
            if (in.in_string[k]) ah[k] += ik * dt / 3600.0f;
            if (!v_ok) continue;
            if (resting && in.t_s - rest_since[k] >= cfg.rest_time_s) {
                soc[k] = vs_soc_from_ocv(&cfg, in.cell_v[k]);  // open-circuit anchor
            } else if (isnan(soc[k])) {
                // first estimate under load: add back the IR drop if the resistance is known
                const float r = in.ir_base[k] > 0.0f ? in.ir_base[k] : 0.0f;
                soc[k] = vs_soc_from_ocv(&cfg, in.cell_v[k] + ik * r);
            } else {
                soc[k] = clamp01(soc[k] - ik * dt / (cfg.capacity_ah * 3600.0f));
            }
        }

        // ---------------- health sub-scores
        for (int k = 0; k < VS_NCELL; ++k) {
            out.sub_ir[k] = out.sub_imb[k] = out.sub_temp[k] = out.sub_gas[k] = NAN;
            if (in.ir_now[k] > 0.0f && in.ir_base[k] > 0.0f)
                out.sub_ir[k] = 100.0f * (1.0f - clamp01((in.ir_now[k] / in.ir_base[k] - 1.0f) / cfg.ir_ratio_span));
            // imbalance among in-string cells, IR drop removed when the baselines are known
            if (in.in_string[k] && !isnan(in.cell_v[k])) {
                float o[VS_NCELL];
                int n = 0;
                for (int j = 0; j < VS_NCELL; ++j)
                    if (j != k && in.in_string[j] && !isnan(in.cell_v[j]))
                        o[n++] = in.cell_v[j] + (in.ir_base[j] > 0.0f ? I * in.ir_base[j] : 0.0f);
                if (n > 0) {
                    const float vk = in.cell_v[k] + (in.ir_base[k] > 0.0f ? I * in.ir_base[k] : 0.0f);
                    out.sub_imb[k] = 100.0f * (1.0f - clamp01(fabsf(vk - median_of(o, n)) / cfg.imbalance_span_v));
                }
            }
            if (in.temp_ok[k]) {
                float o[VS_NCELL];
                int n = 0;
                for (int j = 0; j < VS_NCELL; ++j)
                    if (j != k && in.temp_ok[j]) o[n++] = in.temp_c[j];
                if (n > 0) out.sub_temp[k] = 100.0f * (1.0f - clamp01((in.temp_c[k] - median_of(o, n)) / cfg.temp_span_c));
            }
            if (in.gas_warm) {
                if (isnan(gas0_co[k]) && in.gas_base_co[k] > 0.05f) gas0_co[k] = in.gas_base_co[k];
                if (isnan(gas0_h2[k]) && in.gas_base_h2[k] > 0.05f) gas0_h2[k] = in.gas_base_h2[k];
                if (!isnan(gas0_co[k]) && !isnan(gas0_h2[k])) {
                    const float d = fmaxf(fabsf(in.gas_base_co[k] / gas0_co[k] - 1.0f), fabsf(in.gas_base_h2[k] / gas0_h2[k] - 1.0f));
                    out.sub_gas[k] = 100.0f * (1.0f - clamp01(d / cfg.gas_drift_span));
                }
            }
            const float parts[4] = {out.sub_ir[k], out.sub_imb[k], out.sub_temp[k], out.sub_gas[k]};
            const float w[4] = {cfg.w_ir, cfg.w_imb, cfg.w_temp, cfg.w_gas};
            float sum = 0.0f, wsum = 0.0f;
            for (int i = 0; i < 4; ++i)
                if (!isnan(parts[i])) {
                    sum += w[i] * parts[i];
                    wsum += w[i];
                }
            out.score[k] = wsum >= 0.3f ? sum / wsum : NAN;  // at least ~a third of the weight must be present
            out.soc[k] = soc[k];
            out.ah_out[k] = ah[k];
        }
        out.pack_ah = pack_ah;
        out.pack_wh = pack_wh;
        last_t = in.t_s;
        first = false;
    }
};

}  // namespace

extern "C" {

void vs_health_default_config(vs_health_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->capacity_ah = 2.5f;  // BOM
    const float ocv[11] = {3.00f, 3.45f, 3.55f, 3.62f, 3.68f, 3.74f, 3.82f, 3.90f, 3.98f, 4.08f, 4.20f};  // PLACEHOLDER
    for (int i = 0; i < 11; ++i) c->ocv_table[i] = ocv[i];
    c->rest_current_a = 0.02f;  // PLACEHOLDER
    c->rest_time_s = 120.0f;    // PLACEHOLDER (relaxation time)
    c->ir_ratio_span = 1.0f;    // PLACEHOLDER: doubled resistance = 0 points
    c->imbalance_span_v = 0.10f;
    c->temp_span_c = 8.0f;
    c->gas_drift_span = 0.5f;
    c->w_ir = 0.40f;  // PLACEHOLDER weights
    c->w_imb = 0.25f;
    c->w_temp = 0.25f;
    c->w_gas = 0.10f;
}

size_t vs_sizeof_health_config(void) { return sizeof(vs_health_config_t); }
size_t vs_sizeof_health_in(void) { return sizeof(vs_health_in_t); }
size_t vs_sizeof_health_out(void) { return sizeof(vs_health_out_t); }

void *vs_health_create(const vs_health_config_t *cfg) {
    vs_health_config_t d;
    if (!cfg) {
        vs_health_default_config(&d);
        cfg = &d;
    }
    return new (std::nothrow) Health(*cfg);
}

void vs_health_destroy(void *h) { delete static_cast<Health *>(h); }

void vs_health_step(void *h, const vs_health_in_t *in, vs_health_out_t *out) { static_cast<Health *>(h)->step(*in, *out); }

float vs_soc_from_ocv(const vs_health_config_t *c, float v) {
    if (v <= c->ocv_table[0]) return 0.0f;
    if (v >= c->ocv_table[10]) return 1.0f;
    for (int i = 0; i < 10; ++i)
        if (v < c->ocv_table[i + 1]) return 0.1f * ((float)i + (v - c->ocv_table[i]) / (c->ocv_table[i + 1] - c->ocv_table[i]));
    return 1.0f;
}

}  // extern "C"
