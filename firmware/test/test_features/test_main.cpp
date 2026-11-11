// Native tests for the health, sequence and settings modules:  pio test -e native
#include <math.h>
#include <string.h>
#include <unity.h>

#include "config_table.h"
#include "health.h"
#include "sequences.h"

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------------ health
static vs_health_in_t healthy_in(float t) {
    vs_health_in_t in;
    memset(&in, 0, sizeof in);
    in.t_s = t;
    for (int k = 0; k < 4; ++k) {
        in.cell_v[k] = 3.80f;
        in.in_string[k] = 1;
        in.temp_ok[k] = 1;
        in.temp_c[k] = 28.0f;
        in.gas_base_co[k] = 0.6f;
        in.gas_base_h2[k] = 0.7f;
        in.ir_now[k] = in.ir_base[k] = 0.03f;
    }
    in.current_ok = 1;
    in.gas_warm = 1;
    return in;
}

void test_soc_from_ocv_table() {
    vs_health_config_t c;
    vs_health_default_config(&c);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.40f, vs_soc_from_ocv(&c, 3.68f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.45f, vs_soc_from_ocv(&c, 3.71f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, vs_soc_from_ocv(&c, 2.9f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, vs_soc_from_ocv(&c, 4.25f));
}

void test_coulomb_counting_and_energy() {
    void *h = vs_health_create(nullptr);
    vs_health_out_t out;
    for (int i = 0; i <= 3600; ++i) {  // 1 A for 360 s at 10 Hz
        vs_health_in_t in = healthy_in(i * 0.1f);
        in.pack_current_a = 1.0f;
        in.pack_bus_v = 15.0f;
        in.in_string[3] = 0;  // cell 4 bypassed: carries no current
        vs_health_step(h, &in, &out);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.10f, out.pack_ah);
    TEST_ASSERT_FLOAT_WITHIN(0.03f, 1.50f, out.pack_wh);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.10f, out.ah_out[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out.ah_out[3]);
    vs_health_destroy(h);
}

void test_soc_anchors_after_rest() {
    vs_health_config_t c;
    vs_health_default_config(&c);
    c.rest_time_s = 10.0f;
    void *h = vs_health_create(&c);
    vs_health_out_t out;
    for (int i = 0; i < 200; ++i) {
        vs_health_in_t in = healthy_in(i * 0.1f);
        for (int k = 0; k < 4; ++k) in.cell_v[k] = 3.68f;  // at rest -> SoC 0.40
        vs_health_step(h, &in, &out);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.40f, out.soc[1]);
    vs_health_destroy(h);
}

void test_health_score_equal_cells_is_100() {
    void *h = vs_health_create(nullptr);
    vs_health_out_t out;
    vs_health_in_t in = healthy_in(0);
    vs_health_step(h, &in, &out);
    in.t_s = 0.1f;
    vs_health_step(h, &in, &out);
    for (int k = 0; k < 4; ++k) TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, out.score[k]);
    vs_health_destroy(h);
}

void test_health_score_drops_for_ir_rise_and_heat() {
    void *h = vs_health_create(nullptr);
    vs_health_out_t out;
    vs_health_in_t in = healthy_in(0);
    vs_health_step(h, &in, &out);
    in.t_s = 0.1f;
    in.ir_now[2] = 0.06f;  // doubled
    in.temp_c[2] = 36.0f;  // 8 C above the others
    vs_health_step(h, &in, &out);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, out.sub_ir[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, out.sub_temp[2]);
    TEST_ASSERT_TRUE(out.score[2] < 40.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, out.score[0]);
    vs_health_destroy(h);
}

void test_health_score_unknown_without_data() {
    void *h = vs_health_create(nullptr);
    vs_health_out_t out;
    vs_health_in_t in;
    memset(&in, 0, sizeof in);
    for (int k = 0; k < 4; ++k) in.cell_v[k] = NAN;
    vs_health_step(h, &in, &out);
    TEST_ASSERT_TRUE(isnan(out.score[0]));
    vs_health_destroy(h);
}

// ------------------------------------------------------------------ internal-resistance test
struct IrBench {  // 4 cells at 3.8 V OCV with the given resistances, 10 ohm load
    float r[4] = {0.030f, 0.030f, 0.060f, 0.030f};
    float t = 0.0f;
    bool load = true;
    void sample(vs_irtest_in_t &in) {
        float ocv = 4 * 3.8f, rs = 0.2f;
        for (float x : r) rs += x;
        const float I = load ? ocv / (10.0f + rs) : 0.0f;
        for (int k = 0; k < 4; ++k) {
            in.cell_v[k] = 3.8f - I * r[k];
            in.in_string[k] = 1;
        }
        in.pack_current_a = I;
        in.current_ok = 1;
        in.load_on_now = load;
        in.t_s = t;
    }
};

void test_irtest_measures_each_cell() {
    void *h = vs_irtest_create(nullptr);
    IrBench b;
    vs_irtest_in_t in;
    memset(&in, 0, sizeof in);
    vs_irtest_out_t out;
    bool got = false;
    for (int i = 0; i < 100; ++i) {
        b.t = i * 0.1f;
        b.sample(in);
        in.start = (i == 2);
        vs_irtest_step(h, &in, &out);
        b.load = out.load_override >= 0 ? out.load_override : true;  // override, else the user's request (on)
        if (out.result_new) got = true;
    }
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_UINT8(VS_IRT_DONE, out.state);
    TEST_ASSERT_FLOAT_WITHIN(0.0015f, 0.030f, out.r_ohm[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.0015f, 0.060f, out.r_ohm[2]);
    TEST_ASSERT_EQUAL_INT8(-1, out.load_override);  // load handed back
    TEST_ASSERT_TRUE(b.load);
    vs_irtest_destroy(h);
}

void test_irtest_aborts_on_alarm() {
    void *h = vs_irtest_create(nullptr);
    IrBench b;
    vs_irtest_in_t in;
    memset(&in, 0, sizeof in);
    vs_irtest_out_t out;
    for (int i = 0; i < 60; ++i) {
        b.t = i * 0.1f;
        b.sample(in);
        in.start = (i == 2);
        in.level = (i > 20) ? 1 : 0;
        vs_irtest_step(h, &in, &out);
        if (out.load_override >= 0) b.load = out.load_override;
    }
    TEST_ASSERT_EQUAL_UINT8(VS_IRT_ABORTED, out.state);
    TEST_ASSERT_EQUAL_UINT8(VS_IRT_ABORT_LEVEL, out.abort_reason);
    TEST_ASSERT_EQUAL_INT8(-1, out.load_override);
    vs_irtest_destroy(h);
}

// ------------------------------------------------------------------ power-up relay check
struct RcBench {
    float v[4] = {3.8f, 3.8f, 3.8f, 3.8f};
    int stuck_bypass = -1;    // relay that never energises
    int stuck_energised = -1; // relay that never releases
    bool estop_open = false;
    void sample(const vs_rlycheck_out_t &o, vs_rlycheck_in_t &in) {
        float node = 0.0f;
        for (int k = 0; k < 4; ++k) {
            bool on = o.seg_on[k];
            if (k == stuck_bypass) on = false;
            if (k == stuck_energised) on = true;
            in.tap_v[k] = node + v[k];
            in.tap_ok[k] = 1;
            if (on) node = in.tap_v[k];
        }
        in.bus_v = (o.pack_on && !estop_open) ? node : 0.0f;
        in.bus_ok = 1;
    }
};

static vs_rlycheck_out_t run_rc(RcBench &b, const uint8_t en[4]) {
    void *h = vs_rlycheck_create(nullptr);
    vs_rlycheck_in_t in;
    memset(&in, 0, sizeof in);
    vs_rlycheck_out_t out;
    memset(&out, 0, sizeof out);
    for (int i = 0; i < 200; ++i) {
        in.t_s = i * 0.1f;
        for (int k = 0; k < 4; ++k) in.segment_enabled[k] = en[k];
        b.sample(out, in);
        in.start = (i == 1);
        vs_rlycheck_step(h, &in, &out);
    }
    vs_rlycheck_destroy(h);
    return out;
}

void test_relay_check_passes() {
    RcBench b;
    const uint8_t en[4] = {1, 1, 1, 1};
    vs_rlycheck_out_t o = run_rc(b, en);
    TEST_ASSERT_EQUAL_UINT8(VS_RC_PASS, o.state);
    TEST_ASSERT_EQUAL_UINT8(0, o.active);
}

void test_relay_check_finds_stuck_bypass_relay() {
    RcBench b;
    b.stuck_bypass = 2;
    const uint8_t en[4] = {1, 1, 1, 1};
    vs_rlycheck_out_t o = run_rc(b, en);
    TEST_ASSERT_EQUAL_UINT8(VS_RC_FAIL, o.state);
    TEST_ASSERT_EQUAL_UINT8(VS_RC_RELAY_NO_SWITCH, o.fail_code);
    TEST_ASSERT_EQUAL_INT8(2, o.fail_cell);
    TEST_ASSERT_EQUAL_UINT8(0, o.active);  // relays handed back (all off)
}

void test_relay_check_finds_relay_stuck_energised() {
    RcBench b;
    b.stuck_energised = 1;
    const uint8_t en[4] = {1, 1, 1, 1};
    vs_rlycheck_out_t o = run_rc(b, en);
    TEST_ASSERT_EQUAL_UINT8(VS_RC_FAIL, o.state);
    TEST_ASSERT_EQUAL_UINT8(VS_RC_LIVE_WITH_ALL_BYPASSED, o.fail_code);
}

void test_relay_check_reports_open_pack_path() {
    RcBench b;
    b.estop_open = true;
    const uint8_t en[4] = {1, 1, 1, 1};
    vs_rlycheck_out_t o = run_rc(b, en);
    TEST_ASSERT_EQUAL_UINT8(VS_RC_RELAY_NO_SWITCH, o.fail_code);
    TEST_ASSERT_EQUAL_INT8(0, o.fail_cell);
}

void test_relay_check_skips_dummy_segment() {
    RcBench b;
    b.v[0] = 0.0f;  // dummy cell in segment 1
    const uint8_t en[4] = {0, 1, 1, 1};
    vs_rlycheck_out_t o = run_rc(b, en);
    TEST_ASSERT_EQUAL_UINT8(VS_RC_PASS, o.state);
}

// ------------------------------------------------------------------ test modes
void test_heater_ramp_proportional() {
    int on = 0;
    for (int i = 0; i < 1000; ++i)  // setpoint 40, temp 37.5 -> duty kp * 2.5 = 0.5
        on += vs_heater_ramp(1000.0f + i * 0.01f, 0.0f, 30.0f, 10.0f, 40.0f, 37.5f, 0.2f, 10.0f);
    TEST_ASSERT_INT_WITHIN(15, 500, on);
    TEST_ASSERT_EQUAL(0, vs_heater_ramp(5.0f, 0.0f, 30.0f, 1.0f, 40.0f, 45.0f, 0.2f, 10.0f));  // above setpoint
}

void test_load_profile() {
    TEST_ASSERT_EQUAL(-1, vs_load_profile(0, 10.0f));
    TEST_ASSERT_EQUAL(1, vs_load_profile(1, 10.0f));
    TEST_ASSERT_EQUAL(0, vs_load_profile(1, 22.0f));
    TEST_ASSERT_EQUAL(1, vs_load_profile(1, 50.0f));
    TEST_ASSERT_EQUAL(0, vs_load_profile(1, 77.0f));
}

// ------------------------------------------------------------------ settings table
void test_config_limits() {
    vs_config_t c;
    vs_default_config(&c);
    char err[96];
    TEST_ASSERT_EQUAL(1, vs_config_set(&c, "window_s", 100.0f, err, sizeof err));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, c.window_s);
    TEST_ASSERT_EQUAL(0, vs_config_set(&c, "window_s", 150.0f, err, sizeof err));  // outside spec 60-120
    TEST_ASSERT_EQUAL(0, vs_config_set(&c, "heater_cutoff_c", 60.0f, err, sizeof err));  // above spec 55 C
    TEST_ASSERT_EQUAL(0, vs_config_set(&c, "heater_max_duty", 0.8f, err, sizeof err));   // above spec 50 %
    TEST_ASSERT_EQUAL(0, vs_config_set(&c, "no_such_key", 1.0f, err, sizeof err));
    TEST_ASSERT_EQUAL(1, vs_config_set(&c, "segment_enabled_1", 0.0f, err, sizeof err));
    TEST_ASSERT_EQUAL_INT32(0, c.segment_enabled[0]);
    TEST_ASSERT_EQUAL_INT32(1, c.segment_enabled[1]);
    float v;
    TEST_ASSERT_EQUAL(1, vs_config_get(&c, "gas_cm_reject", &v));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v);
    char json[4096];
    const size_t n = vs_config_json(&c, json, sizeof json);
    TEST_ASSERT_TRUE(n < sizeof json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"heater_cutoff_c\":{\"v\":55"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_soc_from_ocv_table);
    RUN_TEST(test_coulomb_counting_and_energy);
    RUN_TEST(test_soc_anchors_after_rest);
    RUN_TEST(test_health_score_equal_cells_is_100);
    RUN_TEST(test_health_score_drops_for_ir_rise_and_heat);
    RUN_TEST(test_health_score_unknown_without_data);
    RUN_TEST(test_irtest_measures_each_cell);
    RUN_TEST(test_irtest_aborts_on_alarm);
    RUN_TEST(test_relay_check_passes);
    RUN_TEST(test_relay_check_finds_stuck_bypass_relay);
    RUN_TEST(test_relay_check_finds_relay_stuck_energised);
    RUN_TEST(test_relay_check_reports_open_pack_path);
    RUN_TEST(test_relay_check_skips_dummy_segment);
    RUN_TEST(test_heater_ramp_proportional);
    RUN_TEST(test_load_profile);
    RUN_TEST(test_config_limits);
    return UNITY_END();
}
