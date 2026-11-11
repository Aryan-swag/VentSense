// Outputs and buttons are the same for the mock and the real build: they are plain GPIO.
#include <Arduino.h>

#include "hal.h"
#include "pins.h"

vs_outputs_t g_hal_last_out = {};

void hal_gpio_init() {
    // Write LOW before switching to output so no pin ever glitches HIGH (relays are active-HIGH).
    for (int k = 0; k < 4; ++k) {
        digitalWrite(PIN_RLY_SEG[k], LOW);
        pinMode(PIN_RLY_SEG[k], OUTPUT);
    }
    digitalWrite(PIN_RLY_PACK, LOW);
    pinMode(PIN_RLY_PACK, OUTPUT);
    digitalWrite(PIN_HTR_EN, LOW);
    pinMode(PIN_HTR_EN, OUTPUT);
    digitalWrite(PIN_LOAD_EN, LOW);
    pinMode(PIN_LOAD_EN, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_BTN_ACK, INPUT);  // external 10k pull-ups (GPIO 34/35 have no internal pulls)
    pinMode(PIN_BTN_RST, INPUT);
}

void hal_write(const vs_outputs_t &out, float t_s) {
    g_hal_last_out = out;
    for (int k = 0; k < 4; ++k) digitalWrite(PIN_RLY_SEG[k], out.seg_relay_on[k] ? HIGH : LOW);
    digitalWrite(PIN_RLY_PACK, out.pack_relay_on ? HIGH : LOW);
    digitalWrite(PIN_HTR_EN, out.heater_on ? HIGH : LOW);
    digitalWrite(PIN_LOAD_EN, out.load_on ? HIGH : LOW);
    // buzzer patterns: L1 slow beep, L2 fast beep, L3 continuous
    bool bz = false;
    const unsigned ms = (unsigned)(t_s * 1000.0f);
    switch (out.buzzer_mode) {
        case 1: bz = (ms % 2000) < 150; break;
        case 2: bz = (ms % 500) < 250; break;
        case 3: bz = true; break;
        default: bz = false;
    }
    digitalWrite(PIN_BUZZER, bz ? HIGH : LOW);
}

static bool debounced(int pin, uint32_t &since, bool &state) {
    const bool raw = digitalRead(pin) == LOW;  // pressed = LOW
    const uint32_t now = millis();
    if (raw != state) {
        if (since == 0) since = now;
        if (now - since >= 30) {
            state = raw;
            since = 0;
        }
    } else {
        since = 0;
    }
    return state;
}

bool hal_reset_button() {
    static uint32_t since = 0;
    static bool st = false;
    return debounced(PIN_BTN_RST, since, st);
}

bool hal_ack_button() {
    static uint32_t since = 0;
    static bool st = false;
    return debounced(PIN_BTN_ACK, since, st);
}
