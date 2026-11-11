/*
 * VentSense health estimates (smart features P2.2 / P2.3). Hardware-free; runs on the ESP32 and in sim/.
 *
 *  - charge and energy counting from the INA219 (pack) and the relay states (per cell)
 *  - state of charge per cell: anchored on the open-circuit voltage after a rest, coulomb-counted in between
 *  - health score 0-100 per cell: an INDICATOR built from internal-resistance rise, voltage imbalance,
 *    temperature rise against the other cells and gas-baseline drift. Not a certified state of health.
 *
 * ADVISORY ONLY (FEATURES.md rule 3): nothing here ever drives a relay.
 */
#ifndef VS_HEALTH_H
#define VS_HEALTH_H

#include <stddef.h>
#include <stdint.h>

#include "safety_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float capacity_ah;       /* BOM: INR18650-25R 2500 mAh */
    float ocv_table[11];     /* SoC 0, 0.1 .. 1.0 -> V. PLACEHOLDER generic NMC: replace with a measured 25R curve */
    float rest_current_a;    /* |I| below this counts as resting */
    float rest_time_s;       /* rest needed before the OCV anchor */
    float ir_ratio_span;     /* IR / baseline = 1 -> 100 points, 1 + span -> 0 */
    float imbalance_span_v;  /* |V - median of the others| = 0 -> 100, >= span -> 0 */
    float temp_span_c;       /* T - median of the others = 0 -> 100, >= span -> 0 */
    float gas_drift_span;    /* |baseline / first baseline - 1| = 0 -> 100, >= span -> 0 */
    float w_ir, w_imb, w_temp, w_gas; /* weights, renormalised over the parts that are available */
} vs_health_config_t;

typedef struct {
    float t_s;
    float cell_v[VS_NCELL];   /* core output (NaN = invalid) */
    uint8_t in_string[VS_NCELL];
    uint8_t temp_ok[VS_NCELL];
    float temp_c[VS_NCELL];
    float pack_current_a;     /* discharge positive */
    float pack_bus_v;
    uint8_t current_ok;
    uint8_t gas_warm;         /* gas baselines are valid (after MQ warm-up) */
    uint8_t pad_[2];
    float gas_base_co[VS_NCELL];
    float gas_base_h2[VS_NCELL];
    float ir_now[VS_NCELL];   /* latest internal-resistance measurement, ohm, 0 = none */
    float ir_base[VS_NCELL];  /* commissioning baseline, ohm, 0 = none */
} vs_health_in_t;

typedef struct {
    float soc[VS_NCELL];      /* 0..1, NaN = unknown */
    float ah_out[VS_NCELL];   /* Ah discharged by each cell since start (while in the string) */
    float pack_ah;
    float pack_wh;
    float score[VS_NCELL];    /* 0..100 indicator, NaN = not enough data */
    float sub_ir[VS_NCELL];   /* sub-scores 0..100, NaN = not available */
    float sub_imb[VS_NCELL];
    float sub_temp[VS_NCELL];
    float sub_gas[VS_NCELL];
} vs_health_out_t;

void vs_health_default_config(vs_health_config_t *cfg);
size_t vs_sizeof_health_config(void);
size_t vs_sizeof_health_in(void);
size_t vs_sizeof_health_out(void);
void *vs_health_create(const vs_health_config_t *cfg);
void vs_health_destroy(void *h);
void vs_health_step(void *h, const vs_health_in_t *in, vs_health_out_t *out);
/* SoC from an open-circuit voltage via the table (exposed for tests) */
float vs_soc_from_ocv(const vs_health_config_t *cfg, float v);

#ifdef __cplusplus
}
#endif

#endif
