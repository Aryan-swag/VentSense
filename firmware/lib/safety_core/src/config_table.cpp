// Editable settings table: see config_table.h.
#include "config_table.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

enum Kind { F32, I32 };

struct Entry {
    const char *key;
    Kind kind;
    size_t offset;
    float lo, hi;
    const char *unit;
    const char *label;
};

#define F(field, lo, hi, unit, label) {#field, F32, offsetof(vs_config_t, field), lo, hi, unit, label}
#define I(field, lo, hi, unit, label) {#field, I32, offsetof(vs_config_t, field), lo, hi, unit, label}
#define SEG(k) {"segment_enabled_" #k, I32, offsetof(vs_config_t, segment_enabled) + (k - 1) * sizeof(int32_t), 0, 1, "", \
                "Segment " #k " in use (0 = dummy / empty)"}

const Entry TABLE[] = {
    F(window_s, 60, 120, "s", "Correlation window for 2 signal types (spec 60-120 s)"),
    F(debounce_s, 0.2f, 1.0f, "s", "Signal debounce (keep isolation < 1 s after the 2nd signal)"),
    F(gas_warmup_s, 0, 3600, "s", "MQ warm-up before gas votes"),
    F(gas_rise_frac, 0.05f, 3.0f, "x", "Gas rise above baseline (fraction)"),
    F(gas_rate_frac_per_min, 0.02f, 5.0f, "x/min", "Gas rate-of-rise trigger"),
    F(gas_rate_min_rise_frac, 0.0f, 1.0f, "x", "Minimum rise for the rate trigger"),
    F(gas_cm_margin_frac, 0.0f, 2.0f, "x", "Room-gas rejection: margin over the other cells"),
    I(gas_cm_reject, 0, 1, "", "Room-gas (common-mode) rejection on"),
    I(gas_ambient_min_cells, 2, 4, "cells", "Cells rising together = room air"),
    F(gas_baseline_tau_s, 60, 7200, "s", "Gas baseline tracking time constant"),
    F(temp_abs_c, 35, 60, "C", "Temperature absolute threshold (25R limit 60 C)"),
    F(temp_above_ambient_c, 3, 40, "C", "Temperature above room"),
    F(temp_above_neighbors_c, 2, 30, "C", "Temperature above the other cells"),
    F(temp_rate_c_per_min, 0.5f, 20, "C/min", "Temperature rate-of-rise trigger"),
    F(volt_sag_v, 0.02f, 1.0f, "V", "Voltage sag below the other cells"),
    F(cell_uv_v, 2.5f, 3.4f, "V", "Undervoltage: load off below this (25R cut-off 2.5 V)"),
    F(relay_check_tol_v, 0.05f, 1.0f, "V", "Relay verification tolerance"),
    F(path_r_ohm, 0.0f, 1.0f, "ohm", "Pack path resistance (shunt, fuse, relay, wiring)"),
    I(heater_cell, 0, 3, "", "Segment index of the heated (dummy) cell, 0 = segment 1"),
    F(heater_cutoff_c, 30, 55, "C", "Heater cutoff (spec 55 C: may only be lowered)"),
    F(heater_max_duty, 0.05f, 0.5f, "", "Heater duty cap (spec 50 %: may only be lowered)"),
    SEG(1), SEG(2), SEG(3), SEG(4),
};
const int N = sizeof(TABLE) / sizeof(TABLE[0]);

const Entry *find(const char *key) {
    for (int i = 0; i < N; ++i)
        if (strcmp(TABLE[i].key, key) == 0) return &TABLE[i];
    return nullptr;
}

float read(const vs_config_t *c, const Entry &e) {
    const char *p = reinterpret_cast<const char *>(c) + e.offset;
    if (e.kind == F32) {
        float v;
        memcpy(&v, p, sizeof v);
        return v;
    }
    int32_t v;
    memcpy(&v, p, sizeof v);
    return (float)v;
}

}  // namespace

extern "C" {

int vs_config_count(void) { return N; }
const char *vs_config_key(int i) { return (i >= 0 && i < N) ? TABLE[i].key : nullptr; }

int vs_config_set(vs_config_t *c, const char *key, float value, char *err, size_t n) {
    const Entry *e = find(key);
    if (!e) {
        snprintf(err, n, "unknown setting '%s'", key);
        return 0;
    }
    if (isnan(value) || value < e->lo || value > e->hi) {
        snprintf(err, n, "%s must be within %g..%g %s", key, (double)e->lo, (double)e->hi, e->unit);
        return 0;
    }
    char *p = reinterpret_cast<char *>(c) + e->offset;
    if (e->kind == F32) memcpy(p, &value, sizeof value);
    else {
        const int32_t v = (int32_t)lroundf(value);
        memcpy(p, &v, sizeof v);
    }
    if (err && n) err[0] = 0;
    return 1;
}

int vs_config_get(const vs_config_t *c, const char *key, float *value) {
    const Entry *e = find(key);
    if (!e) return 0;
    *value = read(c, *e);
    return 1;
}

size_t vs_config_json(const vs_config_t *c, char *buf, size_t n) {
    size_t o = 0;
    o += snprintf(buf + o, o < n ? n - o : 0, "{");
    for (int i = 0; i < N; ++i) {
        const Entry &e = TABLE[i];
        o += snprintf(buf + o, o < n ? n - o : 0, "%s\"%s\":{\"v\":%g,\"min\":%g,\"max\":%g,\"unit\":\"%s\",\"label\":\"%s\"}",
                      i ? "," : "", e.key, (double)read(c, e), (double)e.lo, (double)e.hi, e.unit, e.label);
    }
    o += snprintf(buf + o, o < n ? n - o : 0, "}");
    return o;
}

}  // extern "C"
