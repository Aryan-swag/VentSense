// Native unit tests for the safety core:  cd firmware && pio test -e native
#include <math.h>
#include <string.h>
#include <unity.h>

#include <initializer_list>

#include "safety_core.h"

namespace {

// A tiny bench: 4 healthy cells at 3.70 V, gas at baseline, 28 C, load 1.48 A.
struct Bench {
    void *core = nullptr;
    vs_config_t cfg;
    vs_inputs_t in;
    vs_outputs_t out;
    float cell_ocv[VS_NCELL] = {3.70f, 3.70f, 3.70f, 3.70f};
    float t = 0.0f;
    int l2_events = 0, l3_events = 0;

    explicit Bench(void (*tweak)(vs_config_t &) = nullptr) {
        vs_default_config(&cfg);
        cfg.gas_warmup_s = 5.0f;  // short warm-up for tests
        if (tweak) tweak(cfg);
        core = vs_create(&cfg);
        memset(&in, 0, sizeof(in));
        memset(&out, 0, sizeof(out));
        for (int k = 0; k < VS_NCELL; ++k) {
            in.cell[k].co_v = 0.60f;
            in.cell[k].h2_v = 0.70f;
            in.cell[k].temp_c = 28.0f;
            in.cell[k].co_ok = in.cell[k].h2_ok = in.cell[k].temp_ok = in.cell[k].tap_ok = 1;
        }
        in.ambient_c = 27.0f;
        in.ambient_rh = 50.0f;
        in.current_ok = in.bus_ok = in.ambient_ok = 1;
        in.armed = 1;
        in.load_request = 1;
    }
    ~Bench() { vs_destroy(core); }

    // physics of the taps: a bypassed cell's + terminal reads node(k-1) + its voltage
    void physics() {
        float node = 0.0f;
        int n_in = 0;
        for (int k = 0; k < VS_NCELL; ++k) {
            in.cell[k].tap_v = node + cell_ocv[k];
            if (out.seg_relay_on[k]) {
                node = in.cell[k].tap_v;
                ++n_in;
            }
        }
        const bool live = out.pack_relay_on && n_in > 0;
        in.pack_current_a = (live && out.load_on) ? node / 10.0f : 0.0f;
        in.pack_bus_v = live ? node - in.pack_current_a * cfg.path_r_ohm : 0.0f;
    }
    void run(float seconds) {
        const int n = (int)lroundf(seconds / 0.1f);
        for (int i = 0; i < n; ++i) {
            physics();
            t += 0.1f;
            in.t_s = t;
            vs_step(core, &in, &out);
            vs_event_t ev;
            while (vs_pop_event(core, &ev)) {
                if (ev.type == VS_EV_L2_TRIP) ++l2_events;
                if (ev.type == VS_EV_L3_TRIP) ++l3_events;
            }
        }
    }
    void press_reset() {
        in.reset_pressed = 1;
        run(0.3f);
        in.reset_pressed = 0;
        run(0.3f);
    }
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_struct_sizes_stable() {
    TEST_ASSERT_EQUAL(sizeof(vs_config_t), vs_sizeof_config());
    TEST_ASSERT_EQUAL(sizeof(vs_inputs_t), vs_sizeof_inputs());
}

void test_normal_operation_no_trip() {
    Bench b;
    b.run(300);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_NORMAL, b.out.level);
    for (int k = 0; k < VS_NCELL; ++k) TEST_ASSERT_EQUAL_UINT8(1, b.out.seg_relay_on[k]);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.pack_relay_on);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.alerts & VS_ALERT_RELAY_MISMATCH);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.70f, b.out.cell_v[2]);
}

void test_not_armed_keeps_all_relays_off() {
    Bench b;
    b.in.armed = 0;
    b.run(20);
    for (int k = 0; k < VS_NCELL; ++k) TEST_ASSERT_EQUAL_UINT8(0, b.out.seg_relay_on[k]);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.pack_relay_on);
    TEST_ASSERT_TRUE(b.out.alerts & VS_ALERT_NOT_ARMED);
}

void test_single_gas_signal_is_level1_only() {
    Bench b;
    b.run(30);
    b.in.cell[2].co_v = 0.95f;  // +58 % on cell 3 only
    b.run(30);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.sig_active[2][VS_SIG_GAS]);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_1, b.out.level);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.seg_relay_on[2]);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.buzzer_mode);
}

void test_gas_then_temp_within_window_trips_only_that_cell() {
    Bench b;
    b.run(30);
    b.in.cell[2].co_v = 0.95f;
    b.run(40);
    b.in.cell[2].temp_c = 52.0f;
    b.run(5);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.cell_latched[2]);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.seg_relay_on[2]);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.seg_relay_on[0]);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.seg_relay_on[1]);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.seg_relay_on[3]);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.pack_relay_on);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_2, b.out.level);
    TEST_ASSERT_EQUAL_UINT8(VS_SIG_GAS, b.out.trip_order[2][0]);
    TEST_ASSERT_EQUAL_UINT8(VS_SIG_TEMP, b.out.trip_order[2][1]);
    TEST_ASSERT_EQUAL(1, b.l2_events);
    // the rest of the pack keeps working: 3 cells through the load
    b.run(2);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 3 * 3.70f / 10.0f, b.in.pack_current_a);
}

void test_second_signal_after_window_does_not_trip() {
    Bench b;
    b.run(30);
    b.in.cell[1].co_v = 0.95f;  // gas pulse for 20 s
    b.run(20);
    b.in.cell[1].co_v = 0.60f;
    b.run(b.cfg.window_s + 20);  // window expires
    b.in.cell[1].temp_c = 52.0f;
    b.run(10);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.cell_latched[1]);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_1, b.out.level);
}

void test_two_cells_level3_opens_pack_relay() {
    Bench b;
    b.run(30);
    for (int k : {1, 2}) {
        b.in.cell[k].co_v = 0.95f;
        b.in.cell[k].temp_c = 52.0f;
    }
    b.run(10);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.pack_latched);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.pack_relay_on);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_3, b.out.level);
    TEST_ASSERT_EQUAL(1, b.l3_events);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.load_on);
}

void test_latch_holds_until_physical_reset() {
    Bench b;
    b.run(30);
    b.in.cell[0].co_v = 0.95f;
    b.in.cell[0].temp_c = 52.0f;
    b.run(10);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.cell_latched[0]);
    b.in.cell[0].co_v = 0.60f;  // conditions clear
    b.in.cell[0].temp_c = 28.0f;
    b.run(200);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.cell_latched[0]);  // still latched
    TEST_ASSERT_EQUAL_UINT8(0, b.out.seg_relay_on[0]);
    b.in.ack_pressed = 1;  // acknowledge silences, does not unlatch
    b.run(0.5f);
    b.in.ack_pressed = 0;
    b.run(0.5f);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.cell_latched[0]);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.buzzer_mode);
    b.press_reset();
    TEST_ASSERT_EQUAL_UINT8(0, b.out.cell_latched[0]);
    b.run(2);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.seg_relay_on[0]);
}

void test_reset_with_fault_still_present_relatches() {
    Bench b;
    b.run(30);
    b.in.cell[3].co_v = 0.95f;
    b.in.cell[3].temp_c = 52.0f;
    b.run(10);
    b.press_reset();
    TEST_ASSERT_EQUAL_UINT8(1, b.out.cell_latched[3]);
}

void test_room_gas_is_rejected_as_common_mode() {
    Bench b;
    b.run(30);
    for (int k = 0; k < VS_NCELL; ++k) b.in.cell[k].co_v = 0.95f;  // incense in the room
    b.run(30);
    for (int k = 0; k < VS_NCELL; ++k) TEST_ASSERT_EQUAL_UINT8(0, b.out.sig_active[k][VS_SIG_GAS]);
    TEST_ASSERT_TRUE(b.out.alerts & VS_ALERT_AMBIENT_GAS);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_1, b.out.level);
    b.in.cell[1].temp_c = 52.0f;  // one temperature signal on top of room gas: still no Level 2
    b.run(10);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.cell_latched[1]);
}

void test_gas_ignored_during_warmup() {
    Bench b([](vs_config_t &c) { c.gas_warmup_s = 120.0f; });
    b.run(10);
    b.in.cell[2].co_v = 0.95f;
    b.in.cell[2].temp_c = 52.0f;
    b.run(30);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.sig_active[2][VS_SIG_GAS]);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.cell_latched[2]);
}

void test_temperature_sensor_fault_flagged_not_counted() {
    Bench b;
    b.run(30);
    b.in.cell[0].temp_ok = 0;  // DS18B20 unplugged
    b.in.cell[0].temp_c = -127.0f;
    b.in.cell[0].co_v = 0.95f;
    b.run(20);
    TEST_ASSERT_TRUE(b.out.cell_faults[0] & VS_FAULT_TEMP);
    TEST_ASSERT_TRUE(b.out.alerts & VS_ALERT_SENSOR_FAULT);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.cell_latched[0]);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_1, b.out.level);
}

void test_open_gas_sensor_is_a_fault() {
    Bench b;
    b.run(30);
    b.in.cell[1].h2_v = 0.0f;
    b.run(1);
    TEST_ASSERT_TRUE(b.out.cell_faults[1] & VS_FAULT_H2);
}

void test_voltage_sag_plus_temperature_trips() {
    Bench b;
    b.run(30);
    b.cell_ocv[1] = 3.45f;  // 250 mV below the others
    b.run(5);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.sig_active[1][VS_SIG_VOLT]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.25f, b.out.volt_dev_v[1]);
    b.in.cell[1].temp_c = 52.0f;
    b.run(5);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.cell_latched[1]);
}

void test_ir_compensation_explains_sag_under_load() {
    // cell 2 has higher internal resistance: its lower voltage under load is explained, no sag signal
    Bench b([](vs_config_t &c) {
        c.ir_ohm[0] = 0.03f;
        c.ir_ohm[1] = 0.10f;
        c.ir_ohm[2] = 0.03f;
        c.ir_ohm[3] = 0.03f;
    });
    b.run(30);
    b.cell_ocv[1] = 3.70f - 0.07f * 1.48f - 0.05f;  // (0.10 - 0.03) x 1.48 A explained, +50 mV unexplained
    b.run(5);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.sig_active[1][VS_SIG_VOLT]);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.05f, b.out.volt_dev_v[1]);
}

void test_bypassed_cell_voltage_still_computed() {
    Bench b;
    b.run(30);
    b.in.cell[1].co_v = 0.95f;
    b.in.cell[1].temp_c = 52.0f;
    b.run(5);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.seg_relay_on[1]);
    b.run(2);
    for (int k = 0; k < VS_NCELL; ++k) TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.70f, b.out.cell_v[k]);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.alerts & VS_ALERT_RELAY_MISMATCH);
}

void test_stuck_relay_detected() {
    Bench b;
    b.run(30);
    // relay 3 commanded in but physically stuck in bypass: tap 3 then reads node 2 + V3,
    // and tap 4 reads node 2 + V4
    for (int i = 0; i < 20; ++i) {
        b.physics();
        b.in.cell[2].tap_v = 7.40f + 3.70f;
        b.in.cell[3].tap_v = 7.40f + 3.70f;
        b.in.pack_bus_v = 11.10f - b.in.pack_current_a * b.cfg.path_r_ohm;
        b.t += 0.1f;
        b.in.t_s = b.t;
        vs_step(b.core, &b.in, &b.out);
    }
    TEST_ASSERT_TRUE(b.out.alerts & VS_ALERT_RELAY_MISMATCH);
}

void test_heater_cutoff_at_55c_and_rearm() {
    Bench b;
    b.in.heater_request = 1;
    b.run(1);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.heater_on);
    b.in.cell[0].temp_c = 55.1f;
    b.run(0.2f);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.heater_on);
    TEST_ASSERT_TRUE(b.out.alerts & VS_ALERT_HEATER_CUTOFF);
    b.in.cell[0].temp_c = 45.0f;  // cooled, but the request is still held: stays off
    b.run(1);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.heater_on);
    b.in.heater_request = 0;  // release, then request again
    b.run(0.5f);
    b.in.heater_request = 1;
    b.run(0.5f);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.heater_on);
}

void test_heater_duty_limited_to_half() {
    Bench b;
    b.in.heater_request = 1;
    int on = 0, n = 0;
    b.run(120);  // settle
    for (int i = 0; i < 6000; ++i) {
        b.run(0.1f);
        on += b.out.heater_on;
        ++n;
    }
    const float duty = (float)on / (float)n;
    TEST_ASSERT_FLOAT_WITHIN(0.06f, 0.475f, duty);
}

void test_heater_off_when_any_cell_latched() {
    Bench b;
    b.in.heater_request = 1;
    b.run(30);
    b.in.cell[3].co_v = 0.95f;
    b.in.cell[3].temp_c = 52.0f;
    b.run(5);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.heater_on);
}

void test_disabled_dummy_segment_never_energised() {
    Bench b([](vs_config_t &c) { c.segment_enabled[0] = 0; });
    b.cell_ocv[0] = 0.0f;  // dummy cell
    b.run(30);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.seg_relay_on[0]);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.seg_relay_on[1]);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.sig_active[0][VS_SIG_VOLT]);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_NORMAL, b.out.level);
}

void test_undervoltage_switches_load_off_until_released() {
    Bench b;
    b.run(30);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.load_on);
    b.cell_ocv[2] = 2.95f;  // below 3.0 V
    b.run(1);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.load_on);
    TEST_ASSERT_TRUE(b.out.alerts & VS_ALERT_UNDERVOLT);
    TEST_ASSERT_EQUAL_UINT8(VS_LEVEL_1, b.out.level);
    b.cell_ocv[2] = 3.30f;  // recovers at rest: still off until the operator releases the request
    b.run(2);
    TEST_ASSERT_EQUAL_UINT8(0, b.out.load_on);
    b.in.load_request = 0;
    b.run(0.5f);
    b.in.load_request = 1;
    b.run(0.5f);
    TEST_ASSERT_EQUAL_UINT8(1, b.out.load_on);
}

void test_ir_estimator() {
    const float vr[4] = {3.80f, 3.80f, 3.80f, 3.80f};
    const float vl[4] = {3.76f, 3.75f, 3.76f, 3.70f};
    float r[4];
    TEST_ASSERT_EQUAL(1, vs_estimate_ir(vr, vl, 0.0f, 1.0f, 0.5f, r));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.04f, r[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.10f, r[3]);
    TEST_ASSERT_EQUAL(0, vs_estimate_ir(vr, vl, 0.0f, 0.2f, 0.5f, r));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_struct_sizes_stable);
    RUN_TEST(test_normal_operation_no_trip);
    RUN_TEST(test_not_armed_keeps_all_relays_off);
    RUN_TEST(test_single_gas_signal_is_level1_only);
    RUN_TEST(test_gas_then_temp_within_window_trips_only_that_cell);
    RUN_TEST(test_second_signal_after_window_does_not_trip);
    RUN_TEST(test_two_cells_level3_opens_pack_relay);
    RUN_TEST(test_latch_holds_until_physical_reset);
    RUN_TEST(test_reset_with_fault_still_present_relatches);
    RUN_TEST(test_room_gas_is_rejected_as_common_mode);
    RUN_TEST(test_gas_ignored_during_warmup);
    RUN_TEST(test_temperature_sensor_fault_flagged_not_counted);
    RUN_TEST(test_open_gas_sensor_is_a_fault);
    RUN_TEST(test_voltage_sag_plus_temperature_trips);
    RUN_TEST(test_ir_compensation_explains_sag_under_load);
    RUN_TEST(test_bypassed_cell_voltage_still_computed);
    RUN_TEST(test_stuck_relay_detected);
    RUN_TEST(test_heater_cutoff_at_55c_and_rearm);
    RUN_TEST(test_heater_duty_limited_to_half);
    RUN_TEST(test_heater_off_when_any_cell_latched);
    RUN_TEST(test_disabled_dummy_segment_never_energised);
    RUN_TEST(test_undervoltage_switches_load_off_until_released);
    RUN_TEST(test_ir_estimator);
    return UNITY_END();
}
