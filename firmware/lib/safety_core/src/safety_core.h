/*
 * VentSense safety core: hardware-free C/C++ (no Arduino, no FreeRTOS).
 *
 * The same code runs in three places:
 *   - on the ESP32, in the safety task (firmware/src)
 *   - in the native unit tests (firmware/test, PlatformIO "native" + Unity)
 *   - in the Python digital twin (sim/), loaded as a shared library via ctypes
 *
 * It implements design spec section 2 (2-of-3 voting, Level 1/2/3, latching until the
 * physical reset) plus the P1 smart features from firmware/FEATURES.md:
 * smart gas (baseline, rate, common-mode rejection, warm-up), smart temperature,
 * voltage sag with IR compensation, sensor faults, relay verification, heater
 * limits (55 C cutoff, 50 % duty cap).
 *
 * All structs are plain C with fixed-size fields, so they can be mirrored in ctypes.
 * Keep the field order in sync with sim/vs_core.py (sizes are checked at load time).
 */
#ifndef VS_SAFETY_CORE_H
#define VS_SAFETY_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_NCELL 4
#define VS_NSIG 3

enum { VS_SIG_GAS = 0, VS_SIG_TEMP = 1, VS_SIG_VOLT = 2 };
enum { VS_LEVEL_NORMAL = 0, VS_LEVEL_1 = 1, VS_LEVEL_2 = 2, VS_LEVEL_3 = 3 };

/* per-cell sensor fault bits */
enum {
    VS_FAULT_CO = 1u << 0,
    VS_FAULT_H2 = 1u << 1,
    VS_FAULT_TEMP = 1u << 2,
    VS_FAULT_TAP = 1u << 3
};

/* global alert bits (vs_outputs_t.alerts) */
enum {
    VS_ALERT_AMBIENT_GAS = 1u << 0,    /* most cells' gas rose together: room air, not a cell   */
    VS_ALERT_RELAY_MISMATCH = 1u << 1, /* measured voltages don't match the commanded relays    */
    VS_ALERT_CURRENT_FAULT = 1u << 2,  /* INA219 not answering                                   */
    VS_ALERT_AMBIENT_FAULT = 1u << 3,  /* BME280 not answering                                   */
    VS_ALERT_HEATER_CUTOFF = 1u << 4,  /* heater cell reached the cutoff (info, no level change) */
    VS_ALERT_HEATER_DUTY = 1u << 5,    /* heater held off by the duty cap (info)                 */
    VS_ALERT_NOT_ARMED = 1u << 6,      /* self-test not passed / not armed: all relays off (info)*/
    VS_ALERT_SENSOR_FAULT = 1u << 7    /* at least one per-cell sensor fault                     */
};

/* event types (vs_event_t.type) */
enum {
    VS_EV_SIGNAL_ON = 1, /* detail = signal type */
    VS_EV_SIGNAL_OFF,    /* detail = signal type */
    VS_EV_L2_TRIP,       /* detail = bit mask of signal types that voted */
    VS_EV_L3_TRIP,       /* detail = bit mask of latched cells */
    VS_EV_RESET,
    VS_EV_FAULT_ON,      /* detail = fault bit */
    VS_EV_FAULT_OFF,     /* detail = fault bit */
    VS_EV_ALERT_ON,      /* detail = alert bit, cell = -1 */
    VS_EV_ALERT_OFF,     /* detail = alert bit, cell = -1 */
    VS_EV_ARMED,
    VS_EV_DISARMED,
    VS_EV_ACK
};

/*
 * Configuration. Values marked PLACEHOLDER in vs_default_config() are first guesses to be
 * tuned on the rig; they are not specifications. Values from the design spec are marked SPEC.
 */
typedef struct {
    float window_s;   /* correlation window for 2 signal types on one cell (SPEC 60-120 s) */
    float debounce_s; /* a raw signal must persist this long to become active / inactive    */

    /* gas: values are ADS1115 input volts (MQ AO after the 15k/22k divider) */
    float gas_warmup_s;           /* MQ heater warm-up: gas excluded from voting before this   */
    float gas_rise_frac;          /* rise above baseline, as a fraction of baseline            */
    float gas_rate_frac_per_min;  /* rate-of-rise trigger (fraction of baseline per minute)    */
    float gas_rate_min_rise_frac; /* the rate trigger also needs at least this much rise       */
    float gas_cm_margin_frac;     /* common-mode: cell rise must beat the others' median by this */
    int32_t gas_cm_reject;        /* 1 = common-mode rejection on                              */
    int32_t gas_ambient_min_cells;/* this many cells rising together = ambient gas alert       */
    float gas_baseline_tau_s;     /* baseline tracking time constant (when quiet)              */
    float gas_min_baseline_v;     /* floor for the baseline used in the fraction               */
    float gas_fault_low_v;        /* below this = sensor open / unpowered                      */
    float gas_fault_high_v;       /* above this = sensor shorted / divider fault               */
    float gas_slope_tau_s;        /* smoothing for the rate estimate                           */

    /* temperature (DS18B20, deg C) */
    float temp_abs_c;
    float temp_above_ambient_c;
    float temp_above_neighbors_c;
    float temp_rate_c_per_min;
    float temp_slope_tau_s;
    float temp_valid_min_c;
    float temp_valid_max_c;

    /* voltage sag (calibrated cell volts) */
    float volt_sag_v;             /* cell below the median of the other in-string cells by this */
    float ir_ohm[VS_NCELL];       /* measured internal resistance per cell, 0 = unknown          */
    float cell_v_min_valid;       /* computed in-string cell voltage below this = relay/tap fault*/
    int32_t segment_enabled[VS_NCELL]; /* 0 = segment never energised (e.g. dummy cell in a test)*/

    /* relay verification */
    float relay_settle_s;         /* ignore voltage checks this long after a relay change        */
    float relay_check_tol_v;      /* allowed |bus - expected|                                    */
    float path_r_ohm;             /* pack relay + fuse + shunt + wiring resistance               */

    /* heater (SPEC 5.1: cut at 55 C on that cell's DS18B20; 50 % duty limit) */
    int32_t heater_cell;
    float heater_cutoff_c;
    float heater_rearm_c;         /* cutoff clears below this, after the request is released     */
    float heater_max_duty;
    float heater_duty_tau_s;
} vs_config_t;

typedef struct {
    float co_v;    /* MQ-7 channel, ADS1115 volts            */
    float h2_v;    /* MQ-8 channel, ADS1115 volts            */
    float temp_c;  /* DS18B20 on the cell wall               */
    float tap_v;   /* calibrated voltage of the cell's + terminal vs pack negative (divider x6) */
    uint8_t co_ok, h2_ok, temp_ok, tap_ok; /* driver says the reading is valid */
} vs_cell_in_t;

typedef struct {
    float t_s; /* monotonic time in seconds */
    vs_cell_in_t cell[VS_NCELL];
    float pack_current_a; /* INA219, discharge positive */
    float pack_bus_v;     /* INA219 bus voltage (load side of the shunt) */
    float ambient_c;      /* BME280 */
    float ambient_rh;     /* BME280 */
    uint8_t current_ok, bus_ok, ambient_ok;
    uint8_t armed;          /* self-test passed and system armed */
    uint8_t reset_pressed;  /* physical reset button (level, edge-detected inside) */
    uint8_t ack_pressed;    /* acknowledge button or dashboard ack (level)          */
    uint8_t heater_request; /* test mode wants the heater on                         */
    uint8_t load_request;   /* test mode wants the load on                           */
} vs_inputs_t;

typedef struct {
    /* commands */
    uint8_t seg_relay_on[VS_NCELL]; /* 1 = energise = cell in string (active-HIGH input) */
    uint8_t pack_relay_on;
    uint8_t heater_on;
    uint8_t load_on;
    uint8_t buzzer_mode; /* 0 off, 1 slow (L1), 2 fast (L2), 3 continuous (L3) */
    /* state */
    uint8_t level;
    uint8_t cell_level[VS_NCELL];
    uint8_t cell_latched[VS_NCELL];
    uint8_t pack_latched;
    uint8_t sig_active[VS_NCELL][VS_NSIG];
    uint8_t cell_faults[VS_NCELL];
    uint8_t trip_order[VS_NCELL][VS_NSIG]; /* signal types in crossing order at the trip, 255 = none */
    uint32_t alerts;
    /* diagnostics for the dashboard / log */
    float cell_v[VS_NCELL];
    float gas_rise_co[VS_NCELL];
    float gas_rise_h2[VS_NCELL];
    float temp_rate_c_per_min[VS_NCELL];
    float volt_dev_v[VS_NCELL];
    float trip_time_s[VS_NCELL]; /* time of the Level 2 latch, -1 = none */
    float pack_trip_time_s;      /* time of the Level 3 latch, -1 = none */
    float heater_duty;
} vs_outputs_t;

typedef struct {
    float t_s;
    int32_t type;
    int32_t cell; /* -1 = global */
    int32_t detail;
} vs_event_t;

void vs_default_config(vs_config_t *cfg);
size_t vs_sizeof_config(void);
size_t vs_sizeof_inputs(void);
size_t vs_sizeof_outputs(void);
size_t vs_sizeof_event(void);

void *vs_create(const vs_config_t *cfg);
void vs_destroy(void *core);
void vs_set_config(void *core, const vs_config_t *cfg);
void vs_step(void *core, const vs_inputs_t *in, vs_outputs_t *out);
int vs_pop_event(void *core, vs_event_t *ev); /* 1 = event returned, 0 = queue empty */

/*
 * Internal resistance per cell from a load step (smart feature P2.1).
 * v_rest/v_load: cell voltages without / with the load; i_rest/i_load: pack current.
 * Returns 1 and fills out_r (ohm) if |di| >= min_di_a, else 0.
 */
int vs_estimate_ir(const float v_rest[VS_NCELL], const float v_load[VS_NCELL], float i_rest, float i_load,
                   float min_di_a, float out_r[VS_NCELL]);

#ifdef __cplusplus
}
#endif

#endif
