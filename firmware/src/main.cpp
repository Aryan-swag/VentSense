// VentSense controller firmware (ESP32-DevKitC-32E, Arduino framework on FreeRTOS).
//
// Task layout (firmware/FEATURES.md section 0):
//   safety task  - core 1, highest app priority, 10 Hz: read sensors -> vs_step -> drive relays.
//                  Registered with the task watchdog; a hang reboots the chip, and GPIOs reset
//                  to high-Z, which releases every relay (fail-secure).
//   comms task   - core 0 (with the Wi-Fi stack): access point, web page + JSON API, serial
//                  console, event log. It only reads a snapshot of the safety state; it can
//                  request the heater/load test modes and acknowledge alarms, never reset a latch.
#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

#include "hal.h"
#include "pins.h"
#include "safety_core.h"

#ifndef VS_AP_SSID
#define VS_AP_SSID "VentSense"
#endif
#ifndef VS_AP_PASS
#define VS_AP_PASS "ventsense-demo"  // CHANGE before any demo (min 8 chars)
#endif

namespace {

void *g_core = nullptr;
vs_config_t g_cfg;
bool g_selftest_ok = false;

// shared between tasks
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
vs_inputs_t g_snap_in;
vs_outputs_t g_snap_out;
volatile bool g_req_heater = false, g_req_load = true, g_req_ack = false, g_operator_armed = true;
QueueHandle_t g_events;

float now_s() { return (float)(esp_timer_get_time() / 1000) / 1000.0f; }

// ------------------------------------------------------------------ safety task
void safety_task(void *) {
    esp_task_wdt_add(nullptr);
    vs_inputs_t in = {};
    vs_outputs_t out = {};
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        hal_read(in);
        in.t_s = now_s();
        in.armed = (g_selftest_ok && g_operator_armed) ? 1 : 0;
        in.reset_pressed = hal_reset_button() ? 1 : 0;  // physical button only
        in.ack_pressed = (hal_ack_button() || g_req_ack) ? 1 : 0;
        in.heater_request = g_req_heater ? 1 : 0;
        in.load_request = g_req_load ? 1 : 0;
        vs_step(g_core, &in, &out);
        hal_write(out, in.t_s);
        if (g_req_ack && !in.ack_pressed) g_req_ack = false;
        vs_event_t ev;
        while (vs_pop_event(g_core, &ev)) xQueueSend(g_events, &ev, 0);
        portENTER_CRITICAL(&g_mux);
        g_snap_in = in;
        g_snap_out = out;
        portEXIT_CRITICAL(&g_mux);
        esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
    }
}

// ------------------------------------------------------------------ comms task
WebServer server(80);

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>VentSense</title>
<style>body{font-family:sans-serif;margin:16px;background:#111;color:#eee}td,th{padding:4px 8px;text-align:right}
.l0{color:#6c6}.l1{color:#fc3}.l2,.l3{color:#f55}</style></head><body>
<h2>VentSense <span id="lvl"></span></h2><table id="t"></table><pre id="raw"></pre>
<script>
async function tick(){try{const s=await (await fetch('/api/state')).json();
document.getElementById('lvl').textContent='Level '+s.level;document.getElementById('lvl').className='l'+s.level;
let h='<tr><th>cell</th><th>V</th><th>T</th><th>CO rise</th><th>H2 rise</th><th>relay</th><th>level</th></tr>';
s.cells.forEach((c,i)=>{h+=`<tr class="l${c.level}"><td>${i+1}</td><td>${c.v.toFixed(3)}</td><td>${c.t.toFixed(1)}</td>`+
`<td>${(c.co*100).toFixed(0)}%</td><td>${(c.h2*100).toFixed(0)}%</td><td>${c.relay?'IN':'BYPASS'}</td><td>${c.level}</td></tr>`});
document.getElementById('t').innerHTML=h;document.getElementById('raw').textContent=
`pack ${s.pack_v.toFixed(2)} V  ${s.pack_i.toFixed(3)} A  relay ${s.pack_relay?'ON':'OFF'}  alerts 0x${s.alerts.toString(16)}`;}catch(e){}}
setInterval(tick,1000);tick();
</script></body></html>)HTML";

void handle_state() {
    vs_inputs_t in;
    vs_outputs_t out;
    portENTER_CRITICAL(&g_mux);
    in = g_snap_in;
    out = g_snap_out;
    portEXIT_CRITICAL(&g_mux);
    String j;
    j.reserve(700);
    j += "{\"t\":" + String(in.t_s, 1) + ",\"level\":" + String(out.level) + ",\"alerts\":" + String(out.alerts) +
         ",\"pack_relay\":" + String(out.pack_relay_on) + ",\"pack_v\":" + String(in.pack_bus_v, 3) +
         ",\"pack_i\":" + String(in.pack_current_a, 3) + ",\"cells\":[";
    for (int k = 0; k < VS_NCELL; ++k) {
        if (k) j += ",";
        const float v = isnan(out.cell_v[k]) ? 0.0f : out.cell_v[k];
        j += "{\"v\":" + String(v, 4) + ",\"t\":" + String(in.cell[k].temp_c, 2) + ",\"co\":" +
             String(out.gas_rise_co[k], 3) + ",\"h2\":" + String(out.gas_rise_h2[k], 3) + ",\"relay\":" +
             String(out.seg_relay_on[k]) + ",\"level\":" + String(out.cell_level[k]) + ",\"faults\":" +
             String(out.cell_faults[k]) + "}";
    }
    j += "]}";
    server.send(200, "application/json", j);
}

void command(const String &c) {
    if (c == "heater on") g_req_heater = true;
    else if (c == "heater off") g_req_heater = false;
    else if (c == "load on") g_req_load = true;
    else if (c == "load off") g_req_load = false;
    else if (c == "ack") g_req_ack = true;
    else if (c == "arm") g_operator_armed = true;
    else if (c == "disarm") g_operator_armed = false;
    else hal_mock_command(c.c_str());  // mock build: inject faults; real build: ignored
}

void handle_cmd() {
    // TODO (P1.1 / rule 5): require the admin password before accepting commands.
    if (server.hasArg("c")) command(server.arg("c"));
    server.send(200, "text/plain", "ok");
}

void comms_task(void *) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(VS_AP_SSID, VS_AP_PASS);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);  // lower TX peaks on the AMS1117 3.3 V rail (smart_features.md section 4)
    server.on("/", [] { server.send_P(200, "text/html", INDEX_HTML); });
    server.on("/api/state", handle_state);
    server.on("/api/cmd", handle_cmd);
    server.begin();
    Serial.printf("AP %s at %s\n", VS_AP_SSID, WiFi.softAPIP().toString().c_str());
    String line;
    for (;;) {
        server.handleClient();
        while (Serial.available()) {
            const char ch = (char)Serial.read();
            if (ch == '\n' || ch == '\r') {
                line.trim();
                if (line.length()) command(line);
                line = "";
            } else {
                line += ch;
            }
        }
        vs_event_t ev;
        while (xQueueReceive(g_events, &ev, 0) == pdTRUE)  // TODO P1.2: also to SD with RTC time
            Serial.printf("EV,%.1f,%d,%d,%d\n", ev.t_s, (int)ev.type, (int)ev.cell, (int)ev.detail);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

void setup() {
    hal_gpio_init();  // first: every output LOW (relays released, heater and load off)
    Serial.begin(115200);
    vs_default_config(&g_cfg);  // TODO P1.7: load calibration + thresholds from NVS
    g_core = vs_create(&g_cfg);
    g_events = xQueueCreate(64, sizeof(vs_event_t));
    g_selftest_ok = hal_init();
    Serial.printf("VentSense firmware, mock sensors=%d, self-test %s\n", (int)VS_MOCK_SENSORS,
                  g_selftest_ok ? "PASSED" : "FAILED (relays stay released)");

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wc = {};
    wc.timeout_ms = 2000;
    wc.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wc) != ESP_OK) esp_task_wdt_init(&wc);
#else
    esp_task_wdt_init(2, true);  // 2 s, panic -> reboot -> relays released
#endif

    xTaskCreatePinnedToCore(safety_task, "safety", 6144, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
    xTaskCreatePinnedToCore(comms_task, "comms", 8192, nullptr, 2, nullptr, 0);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
