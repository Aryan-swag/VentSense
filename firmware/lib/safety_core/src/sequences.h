/*
 * VentSense timed sequences. Hardware-free; runs on the ESP32 and in sim/.
 *
 *  vs_irtest   - per-cell internal resistance from a load step (P2.1): R = dV / dI at 1 s.
 *  vs_rlycheck - power-up relay check (P1.8): pack relay on, then each bypass relay alone, then all,
 *                comparing the INA219 bus voltage with the cell taps. Arming waits for a PASS.
 *  vs_heater_ramp - time-proportioning heater controller for repeatable thermal tests (P2.5a).
 *  vs_load_profile - load on/off pattern imitating a scooter ride (P2.5b).
 *
 * The safety core still has the last word: it cuts the heater at 55 C / 50 % duty and drops relays on a
 * trip whatever these sequences request.
 */
#ifndef VS_SEQUENCES_H
#define VS_SEQUENCES_H

#include <stddef.h>
#include <stdint.h>

#include "safety_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ internal resistance test */
enum { VS_IRT_IDLE = 0, VS_IRT_PRE, VS_IRT_STEP, VS_IRT_DONE, VS_IRT_ABORTED };
enum { VS_IRT_OK = 0, VS_IRT_ABORT_LEVEL, VS_IRT_ABORT_RELAYS, VS_IRT_ABORT_NO_DI, VS_IRT_ABORT_CURRENT, VS_IRT_ABORT_FEW };

typedef struct {
    float pre_s;    /* hold the present load state this long, averaging the end of it        */
    float step_s;   /* time after the step at which the second sample is taken (DCIR at 1 s)  */
    float avg_s;    /* averaging window at the end of each phase                              */
    float min_di_a; /* the step must change the current by at least this much                 */
} vs_irtest_config_t;

typedef struct {
    float t_s;
    float cell_v[VS_NCELL];
    uint8_t in_string[VS_NCELL];
    float pack_current_a;
    uint8_t current_ok;
    uint8_t level;       /* core system level: the test runs and continues only at Level 0 */
    uint8_t load_on_now; /* core output load_on */
    uint8_t start;       /* request (edge) */
} vs_irtest_in_t;

typedef struct {
    int8_t load_override; /* -1 = none, 0 = force off, 1 = force on */
    uint8_t state;
    uint8_t result_new;   /* 1 for one step when a result is ready */
    uint8_t abort_reason;
    float r_ohm[VS_NCELL]; /* last result, NaN for cells not in the string */
    float di_a;
} vs_irtest_out_t;

size_t vs_sizeof_irtest_in(void);
size_t vs_sizeof_irtest_out(void);
void vs_irtest_default_config(vs_irtest_config_t *c);
void *vs_irtest_create(const vs_irtest_config_t *c);
void vs_irtest_destroy(void *h);
void vs_irtest_step(void *h, const vs_irtest_in_t *in, vs_irtest_out_t *out);

/* ------------------------------------------------------------------ power-up relay check */
enum { VS_RC_IDLE = 0, VS_RC_RUNNING, VS_RC_PASS, VS_RC_FAIL };
enum {
    VS_RC_OK = 0,
    VS_RC_LIVE_WITH_ALL_BYPASSED, /* output live with every cell bypassed: a bypass relay stuck, or wiring */
    VS_RC_CELL_MISSING,           /* tap of the cell under test out of range: cell missing / tap wiring     */
    VS_RC_RELAY_NO_SWITCH,        /* bus does not follow the cell under test: relay, pack path or E-stop    */
    VS_RC_ALL_ON_MISMATCH,        /* all relays on, bus does not match the top tap                          */
    VS_RC_NO_BUS                  /* INA219 bus voltage not available                                       */
};

typedef struct {
    float dwell_s;    /* wait after each relay change before measuring */
    float tol_v;      /* |bus - expected| allowed */
    float zero_tol_v; /* bus must be below this with every cell bypassed */
    float min_cell_v; /* plausible cell range for the cell under test */
    float max_cell_v;
} vs_rlycheck_config_t;

typedef struct {
    float t_s;
    float tap_v[VS_NCELL];
    uint8_t tap_ok[VS_NCELL];
    uint8_t segment_enabled[VS_NCELL];
    float bus_v;
    uint8_t bus_ok;
    uint8_t start; /* request (edge) */
    uint8_t pad_[2];
} vs_rlycheck_in_t;

typedef struct {
    uint8_t active;              /* 1 = the sequence owns the relays */
    uint8_t seg_on[VS_NCELL];
    uint8_t pack_on;
    uint8_t state;               /* VS_RC_* */
    int8_t phase;                /* -1 all off, 0..3 cell k alone, 4 all on */
    uint8_t fail_code;
    int8_t fail_cell;            /* 0-based, -1 = none */
    uint8_t pad_[2];
    float measured_v, expected_v; /* at the failing step */
} vs_rlycheck_out_t;

void vs_rlycheck_default_config(vs_rlycheck_config_t *c);
void *vs_rlycheck_create(const vs_rlycheck_config_t *c);
void vs_rlycheck_destroy(void *h);
void vs_rlycheck_step(void *h, const vs_rlycheck_in_t *in, vs_rlycheck_out_t *out);

/* ------------------------------------------------------------------ test modes */
/* Heater ramp: setpoint = min(target, T0 + rate * (t - t0)); duty = kp * (setpoint - T), time-proportioned
 * over window_s. Returns the heater request (0/1). Pass t0/T0 from when the ramp started. */
int vs_heater_ramp(float t, float t0, float temp0_c, float rate_c_per_min, float target_c, float temp_c, float kp,
                   float window_s);
/* Load profile: 0 = none (returns -1), 1 = scooter-like on/off pattern with an 80 s cycle. */
int vs_load_profile(int profile, float t);

#ifdef __cplusplus
}
#endif

#endif
