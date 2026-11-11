// Real sensor drivers for the rig (VS_MOCK_SENSORS=0).
//
// Pin map / addresses: include/pins.h (design spec 5.4 / 5.5). Not yet run on hardware: every
// module-specific assumption is marked VERIFY and must be checked when the modules arrive.
#if !VS_MOCK_SENSORS
#include <Adafruit_ADS1X15.h>
#include <Adafruit_BME280.h>
#include <Adafruit_INA219.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Preferences.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hal.h"
#include "pins.h"

namespace {

SemaphoreHandle_t i2c_mtx = nullptr;
SemaphoreHandle_t ow_mtx = nullptr;  // 1-Wire: safety task services it, the 'probes' command borrows it
struct I2cLock {
    bool ok;
    I2cLock() { ok = xSemaphoreTake(i2c_mtx, pdMS_TO_TICKS(250)) == pdTRUE; }
    ~I2cLock() {
        if (ok) xSemaphoreGive(i2c_mtx);
    }
};

Adafruit_ADS1115 ads[3];  // 0 = CO (0x48), 1 = H2 (0x49), 2 = cell taps (0x4B)
const uint8_t ADS_ADDR[3] = {I2C_ADS_CO, I2C_ADS_H2, I2C_ADS_CELL};
bool ads_ok[3] = {false, false, false};
Adafruit_INA219 ina(I2C_INA219);
bool ina_ok = false;
Adafruit_BME280 bme;
bool bme_ok = false;
RTC_DS3231 rtc;
bool rtc_ok = false, rtc_lost = false;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oled_ok = false;
bool pcf_ok[2] = {false, false};
uint8_t pcf_last[2] = {0xFF, 0xFF};

OneWire ow(PIN_ONEWIRE);
DallasTemperature ds(&ow);
DeviceAddress found[8];
int n_found = 0;
DeviceAddress probe_rom[4];
bool map_ok = false;  // probe-to-cell mapping confirmed (stored in NVS)
float ds_temp[4] = {NAN, NAN, NAN, NAN};
bool ds_valid[4] = {false, false, false, false};
uint32_t ds_requested_ms = 0;
bool ds_pending = false;

// tap calibration (P1.7): calibrated = gain * raw + offset, raw = ADS volts x 6
float cal_gain[4] = {1, 1, 1, 1}, cal_off[4] = {0, 0, 0, 0};
float cal_pt_raw[4][2], cal_pt_meter[4][2];
int cal_npt[4] = {0, 0, 0, 0};
volatile float last_tap_raw[4] = {0, 0, 0, 0};

float bme_t = NAN, bme_rh = NAN;
uint32_t bme_next_ms = 0;
int presence_rr = 0;
bool sd_ok = false;
File log_file;
char log_name[32] = "";
uint32_t tc_next_ms = 0;
float tc_last = NAN;
Preferences prefs;

bool present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void rom_str(const DeviceAddress a, char *buf) {
    for (int i = 0; i < 8; ++i) sprintf(buf + 2 * i, "%02X", a[i]);
}

void load_nvs() {
    prefs.begin("vs", true);
    map_ok = prefs.getBool("map_ok", false);
    for (int k = 0; k < 4; ++k) {
        char key[8];
        snprintf(key, sizeof key, "rom%d", k);
        if (prefs.getBytes(key, probe_rom[k], 8) != 8) map_ok = false;
        snprintf(key, sizeof key, "cg%d", k);
        cal_gain[k] = prefs.getFloat(key, 1.0f);
        snprintf(key, sizeof key, "co%d", k);
        cal_off[k] = prefs.getFloat(key, 0.0f);
    }
    prefs.end();
}

void save_map() {
    prefs.begin("vs", false);
    for (int k = 0; k < 4; ++k) {
        char key[8];
        snprintf(key, sizeof key, "rom%d", k);
        prefs.putBytes(key, probe_rom[k], 8);
    }
    prefs.putBool("map_ok", map_ok);
    prefs.end();
}

void save_cal(int k) {
    prefs.begin("vs", false);
    char key[8];
    snprintf(key, sizeof key, "cg%d", k);
    prefs.putFloat(key, cal_gain[k]);
    snprintf(key, sizeof key, "co%d", k);
    prefs.putFloat(key, cal_off[k]);
    prefs.end();
}

void ds_discover() {
    ds.begin();
    ds.setWaitForConversion(false);
    n_found = 0;
    for (int i = 0; i < ds.getDeviceCount() && n_found < 8; ++i)
        if (ds.getAddress(found[n_found], i)) {
            ds.setResolution(found[n_found], 12);
            ++n_found;
        }
    // without a stored mapping, use discovery order but do NOT confirm it
    if (!map_ok)
        for (int k = 0; k < 4 && k < n_found; ++k) memcpy(probe_rom[k], found[k], 8);
}

bool mapped_probes_present() {
    for (int k = 0; k < 4; ++k) {
        bool hit = false;
        for (int i = 0; i < n_found; ++i)
            if (memcmp(found[i], probe_rom[k], 8) == 0) hit = true;
        if (!hit) return false;
    }
    return true;
}

void ds_service() {  // non-blocking: all 4 conversions together, read after 750 ms (DS18B20 12-bit max)
    if (xSemaphoreTake(ow_mtx, 0) != pdTRUE) return;  // a console command is using the bus: skip this cycle
    struct Give {
        ~Give() { xSemaphoreGive(ow_mtx); }
    } give;
    const uint32_t now = millis();
    if (!ds_pending) {
        ds.requestTemperatures();
        ds_requested_ms = now;
        ds_pending = true;
        return;
    }
    if (now - ds_requested_ms < 760) return;
    for (int k = 0; k < 4; ++k) {
        const float t = ds.getTempC(probe_rom[k]);
        bool ok = t != DEVICE_DISCONNECTED_C;
        // 85.0 C is the power-on reset value: invalid unless the previous reading was already near it
        if (ok && t == 85.0f && !(ds_valid[k] && fabsf(ds_temp[k] - 85.0f) < 5.0f)) ok = false;
        ds_valid[k] = ok;
        if (ok) ds_temp[k] = t;
    }
    ds_pending = false;
}

}  // namespace

// ------------------------------------------------------------------ init + self-test
bool hal_init(char *report, size_t n) {
    i2c_mtx = xSemaphoreCreateMutex();
    ow_mtx = xSemaphoreCreateMutex();
    Wire.begin(PIN_SDA, PIN_SCL, 100000);  // 100 kHz: the PCF8574 limit
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
    pinMode(PIN_TC_CS, OUTPUT);
    digitalWrite(PIN_TC_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    load_nvs();
    {
        I2cLock l;
        for (int i = 0; i < 3; ++i) {
            ads_ok[i] = ads[i].begin(ADS_ADDR[i], &Wire);
            if (ads_ok[i]) {
                ads[i].setGain(GAIN_ONE);                  // +-4.096 V (design spec 5.3)
                ads[i].setDataRate(RATE_ADS1115_475SPS);   // ~36 ms for all 12 channels
            }
        }
        ina_ok = ina.begin(&Wire);
        if (ina_ok) ina.setCalibration_32V_2A();  // 0.1 ohm shunt, 3.2 A range (design spec 5.2)
        bme_ok = bme.begin(I2C_BME280, &Wire);     // fails on a BMP280 (no humidity): VERIFY the chip
        rtc_ok = rtc.begin(&Wire);
        rtc_lost = rtc_ok && rtc.lostPower();
        oled_ok = present(I2C_OLED) && oled.begin(SSD1306_SWITCHCAPVCC, I2C_OLED);
        pcf_ok[0] = present(I2C_PCF_1);
        pcf_ok[1] = present(I2C_PCF_2);
        for (int c = 0; c < 2; ++c)
            if (pcf_ok[c]) {
                Wire.beginTransmission(c ? I2C_PCF_2 : I2C_PCF_1);
                Wire.write(0xFF);  // all LEDs off (sink-driven: 0 = on)
                Wire.endTransmission();
            }
    }
    ds_discover();
    return hal_selftest(report, n);
}

bool hal_selftest(char *r, size_t n) {
    {
        I2cLock l;
        for (int i = 0; i < 3; ++i) ads_ok[i] = ads_ok[i] && present(ADS_ADDR[i]);
        ina_ok = ina_ok && present(I2C_INA219);
        bme_ok = bme_ok && present(I2C_BME280);
        pcf_ok[0] = present(I2C_PCF_1);
        pcf_ok[1] = present(I2C_PCF_2);
    }
    const bool probes_ok = n_found >= 4 && map_ok && mapped_probes_present();
    const bool safe = ads_ok[0] && ads_ok[1] && ads_ok[2] && ina_ok && probes_ok;
    snprintf(r, n,
             "SELF-TEST %s\n"
             " ADS1115 CO 0x48 %s | H2 0x49 %s | cells 0x4B %s\n"
             " INA219 0x40 %s | DS18B20 found %d, mapping %s\n"
             " BME280 0x76 %s | DS3231 %s | OLED %s | PCF8574 0x20 %s 0x21 %s\n"
             "%s",
             safe ? "PASSED: arming allowed" : "FAILED: relays stay released", ads_ok[0] ? "OK" : "MISSING",
             ads_ok[1] ? "OK" : "MISSING", ads_ok[2] ? "OK" : "MISSING", ina_ok ? "OK" : "MISSING", n_found,
             probes_ok ? "confirmed" : (map_ok ? "probe missing" : "NOT CONFIRMED (use 'probes' then 'probe K N')"),
             bme_ok ? "OK" : "missing or BMP280 (warn)", rtc_ok ? (rtc_lost ? "time not set (warn)" : "OK") : "missing (warn)",
             oled_ok ? "OK" : "missing (warn)", pcf_ok[0] ? "OK" : "missing", pcf_ok[1] ? "OK" : "missing",
             sd_ok ? " microSD OK\n" : " microSD not mounted (warn)\n");
    return safe;
}

// ------------------------------------------------------------------ safety task
void hal_read(vs_inputs_t &in) {
    {
        I2cLock l;
        if (l.ok) {
            // presence: one device per cycle, so a module that drops out is flagged within ~0.5 s
            const int dev = presence_rr++ % 5;
            if (dev < 3) ads_ok[dev] = present(ADS_ADDR[dev]);
            else if (dev == 3) ina_ok = present(I2C_INA219);
            else bme_ok = present(I2C_BME280);
            for (int k = 0; k < 4; ++k) {
                vs_cell_in_t &c = in.cell[k];
                c.co_ok = ads_ok[0];
                c.h2_ok = ads_ok[1];
                c.tap_ok = ads_ok[2];
                if (ads_ok[0]) c.co_v = ads[0].computeVolts(ads[0].readADC_SingleEnded(k));  // A0..A3 = GAS1..4_CO
                if (ads_ok[1]) c.h2_v = ads[1].computeVolts(ads[1].readADC_SingleEnded(k));  // A0..A3 = GAS1..4_H2
                if (ads_ok[2]) {
                    const float raw = ads[2].computeVolts(ads[2].readADC_SingleEnded(k)) * 6.0f;  // 100k/20k
                    last_tap_raw[k] = raw;
                    c.tap_v = cal_gain[k] * raw + cal_off[k];
                }
            }
            in.current_ok = in.bus_ok = ina_ok;
            if (ina_ok) {
                in.pack_current_a = ina.getCurrent_mA() / 1000.0f;  // VERIFY sign: VIN+ pack side = discharge positive
                in.pack_bus_v = ina.getBusVoltage_V();
            }
            if (bme_ok && millis() >= bme_next_ms) {
                bme_t = bme.readTemperature();
                bme_rh = bme.readHumidity();
                bme_next_ms = millis() + 1000;
            }
            in.ambient_ok = bme_ok && !isnan(bme_t);
            in.ambient_c = bme_t;
            in.ambient_rh = bme_rh;
        } else {  // bus stuck: report everything invalid so the core raises faults
            for (int k = 0; k < 4; ++k) in.cell[k].co_ok = in.cell[k].h2_ok = in.cell[k].tap_ok = 0;
            in.current_ok = in.bus_ok = in.ambient_ok = 0;
        }
    }
    ds_service();
    for (int k = 0; k < 4; ++k) {
        in.cell[k].temp_ok = ds_valid[k] && map_ok;
        in.cell[k].temp_c = ds_valid[k] ? ds_temp[k] : -127.0f;
    }
}

void hal_leds(const vs_outputs_t &out, bool armed) {
    // PCF8574 #1 = segments 1-2, #2 = segments 3-4; P0/P1/P2 = red/yellow/green of the first segment,
    // P3/P4/P5 = the second; P6/P7 unused (decision 25). Sink-driven: write 0 to light an LED.
    uint8_t b[2] = {0xFF, 0xFF};
    for (int k = 0; k < 4; ++k) {
        int bit;  // 0 red, 1 yellow, 2 green
        if (!armed) bit = 1;
        else if (out.cell_latched[k] || !out.seg_relay_on[k]) bit = 0;
        else bit = out.cell_level[k] >= 1 ? 1 : 2;
        b[k / 2] &= (uint8_t) ~(1u << ((k % 2) * 3 + bit));
    }
    for (int c = 0; c < 2; ++c) {
        if (!pcf_ok[c] || b[c] == pcf_last[c]) continue;
        I2cLock l;
        if (!l.ok) continue;
        Wire.beginTransmission(c ? I2C_PCF_2 : I2C_PCF_1);
        Wire.write(b[c]);
        if (Wire.endTransmission() == 0) pcf_last[c] = b[c];
    }
}

// ------------------------------------------------------------------ comms task
void hal_ui_update(const vs_inputs_t &in, const vs_outputs_t &out, bool armed) {
    if (!oled_ok) return;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.printf("VentSense L%d %s\n", out.level, armed ? "ARMED" : "NOT ARMED");
    for (int k = 0; k < 4; ++k) {
        const float v = isnan(out.cell_v[k]) ? 0.0f : out.cell_v[k];
        oled.printf("C%d %5.3fV %4.1fC %s%s\n", k + 1, v, in.cell[k].temp_c, out.seg_relay_on[k] ? "IN" : "BY",
                    out.cell_latched[k] ? "!" : "");
    }
    oled.printf("Pack %5.2fV %5.3fA\n", in.pack_bus_v, in.pack_current_a);
    oled.printf("Alerts 0x%02lX\n", (unsigned long)out.alerts);
    I2cLock l;  // a full frame is ~1 KB: about 100 ms at 100 kHz; the safety task waits at most that long
    if (l.ok) oled.display();
}

float hal_tc_read() {
    if (millis() < tc_next_ms) return tc_last;  // MAX6675 conversion time 0.22 s
    tc_next_ms = millis() + 250;
    uint16_t v;
#if defined(VS_TC_ON_IO36) && VS_TC_ON_IO36
    // JP2 moved to 2-3: SO on IO36, bit-banged (SD idle, its CS high)
    pinMode(PIN_TC_SO_ALT, INPUT);
    digitalWrite(PIN_TC_CS, LOW);
    delayMicroseconds(2);
    v = 0;
    for (int i = 15; i >= 0; --i) {
        digitalWrite(PIN_SPI_SCK, HIGH);
        delayMicroseconds(1);
        if (digitalRead(PIN_TC_SO_ALT)) v |= (uint16_t)(1u << i);
        digitalWrite(PIN_SPI_SCK, LOW);
        delayMicroseconds(1);
    }
    digitalWrite(PIN_TC_CS, HIGH);
#else
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));  // MAX6675 SCK max 4.3 MHz
    digitalWrite(PIN_TC_CS, LOW);
    delayMicroseconds(2);
    v = SPI.transfer16(0);
    digitalWrite(PIN_TC_CS, HIGH);
    SPI.endTransaction();
#endif
    if (v == 0xFFFF || v == 0x0000 || (v & 0x4)) tc_last = NAN;  // bus floating / no module / open thermocouple
    else tc_last = (v >> 3) * 0.25f;
    return tc_last;
}

void hal_timestamp(char *buf, size_t n) {
    if (rtc_ok && !rtc_lost) {
        I2cLock l;
        if (l.ok) {
            const DateTime t = rtc.now();
            snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
            return;
        }
    }
    snprintf(buf, n, "uptime %lu s", (unsigned long)(millis() / 1000));
}

bool hal_log_begin() {
    sd_ok = SD.begin(PIN_SD_CS, SPI, 4000000);
    if (!sd_ok) return false;
    char ts[32];
    hal_timestamp(ts, sizeof ts);
    if (rtc_ok && !rtc_lost) snprintf(log_name, sizeof log_name, "/vs_%.4s%.2s%.2s.csv", ts, ts + 5, ts + 8);
    else snprintf(log_name, sizeof log_name, "/vs_boot.csv");
    log_file = SD.open(log_name, FILE_APPEND);
    return (bool)log_file;
}

void hal_log(const char *line) {
    if (sd_ok && log_file) log_file.println(line);
}

void hal_log_flush() {
    if (sd_ok && log_file) log_file.flush();
}

void hal_command(const char *cmd, char *reply, size_t n) {
    int k = 0, idx = 0;
    float v = 0.0f;
    reply[0] = 0;
    if (strcmp(cmd, "probes") == 0) {
        xSemaphoreTake(ow_mtx, portMAX_DELAY);
        ds_discover();
        size_t off = snprintf(reply, n, "DS18B20 found %d (warm one probe by hand to identify it):\n", n_found);
        ds.setWaitForConversion(true);
        ds.requestTemperatures();
        ds.setWaitForConversion(false);
        for (int i = 0; i < n_found && off < n; ++i) {
            char rom[17];
            rom_str(found[i], rom);
            off += snprintf(reply + off, n - off, " N=%d ROM %s  %.2f C\n", i, rom, ds.getTempC(found[i]));
        }
        ds_pending = false;
        xSemaphoreGive(ow_mtx);
    } else if (sscanf(cmd, "probe %d %d", &k, &idx) == 2 && k >= 1 && k <= 4 && idx >= 0 && idx < n_found) {
        memcpy(probe_rom[k - 1], found[idx], 8);
        static bool assigned[4] = {false, false, false, false};
        assigned[k - 1] = true;
        map_ok = assigned[0] && assigned[1] && assigned[2] && assigned[3];
        if (map_ok) save_map();
        snprintf(reply, n, "cell %d <- probe N=%d. %s", k, idx,
                 map_ok ? "All 4 assigned and saved; run 'selftest'." : "Assign all 4 cells to save the mapping.");
    } else if (sscanf(cmd, "calpt %d %f", &k, &v) == 2 && k >= 1 && k <= 4) {
        // multimeter: cell k + terminal (CELLk_P) to pack negative, in volts
        const int i = k - 1;
        const int p = cal_npt[i] < 2 ? cal_npt[i] : 1;
        cal_pt_raw[i][p] = last_tap_raw[i];
        cal_pt_meter[i][p] = v;
        cal_npt[i] = p + 1;
        if (cal_npt[i] == 1 || fabsf(cal_pt_raw[i][1] - cal_pt_raw[i][0]) < 0.5f) {
            cal_gain[i] = cal_pt_raw[i][p] > 0.1f ? v / cal_pt_raw[i][p] : 1.0f;  // one point: gain only
            cal_off[i] = 0.0f;
        } else {
            cal_gain[i] = (cal_pt_meter[i][1] - cal_pt_meter[i][0]) / (cal_pt_raw[i][1] - cal_pt_raw[i][0]);
            cal_off[i] = cal_pt_meter[i][0] - cal_gain[i] * cal_pt_raw[i][0];
        }
        save_cal(i);
        snprintf(reply, n, "tap %d: point %d raw %.4f V meter %.4f V -> gain %.5f offset %.4f (saved)", k, cal_npt[i],
                 (double)cal_pt_raw[i][p], (double)v, (double)cal_gain[i], (double)cal_off[i]);
    } else if (strcmp(cmd, "calshow") == 0) {
        size_t off = 0;
        for (int i = 0; i < 4 && off < n; ++i)
            off += snprintf(reply + off, n - off, "tap %d gain %.5f offset %.4f raw now %.4f V\n", i + 1, (double)cal_gain[i],
                            (double)cal_off[i], (double)last_tap_raw[i]);
    } else if (sscanf(cmd, "calclear %d", &k) == 1 && k >= 1 && k <= 4) {
        cal_gain[k - 1] = 1.0f;
        cal_off[k - 1] = 0.0f;
        cal_npt[k - 1] = 0;
        save_cal(k - 1);
        snprintf(reply, n, "tap %d calibration cleared", k);
    }
}
#endif
