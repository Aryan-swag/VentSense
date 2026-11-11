# VentSense — smart firmware features (agreed 11 Nov 2026)

Scope: features that run on the **existing v1.1 hardware**, with no PCB or BOM change. Hardware reference: the design specification and the design decision log.
Status of every item: **planned** unless marked below. Mark it done here when it's implemented and tested.

**Progress (11 Nov 2026):**
- Build-order steps 1 and 2 are done.
  - **Safety core** in `firmware/lib/safety_core`: voting, latching, P1.3, P1.4, P1.5, P1.6 logic, heater limits and the IR estimator. 22/22 native unit tests pass.
  - **Digital twin** in `sim/`: 29/29 scenario checks pass. Monte Carlo: 0/40 false trips, 40/40 failing cells isolated, max latency 0.6 s after the 2nd signal.
- Step 3 is done: the **ESP32 skeleton** (tasks, watchdog, mock sensors, basic web page + JSON API) compiles.
- Step 4 is written and compiles, but is **untested on hardware**. Real drivers for all modules, plus:
  - P1.8 self-test (arming needs 3 × ADS1115, the INA219 and 4 mapped DS18B20s);
  - P2.8 LEDs and OLED;
  - 1 Hz SD logging (P1.2 base);
  - tap calibration commands (P1.7 base).
  - Bring-up steps are in `BRINGUP.md`.
- **11 Nov (later): every P1 and P2 feature is implemented.** 39 unit tests pass and both ESP32 builds compile. The digital twin passes 35/35 checks, including the internal-resistance test (within 2 % of the model) and the health indicator flagging an aged cell. The dashboard was tested in a browser against the twin (`sim/dash_server.py`).
  - **Status of each feature: implemented, not yet run on hardware.**
  - **New P1.9:** undervoltage load cutoff. It was found in the twin: the rig has no BMS, so the 10 Ω load could over-discharge the cells.
  - **P3 (ML, trend prediction, cloud):** not started.

## 0. Safety rules (apply to every feature)

1. **The safety core never depends on Wi-Fi or the dashboard.** 2-of-3 voting, latching, relay control, the heater cutoff and the watchdog run in their own high-priority task (ESP32 core 1). Wi-Fi, the web dashboard, logging and alerts run on core 0 and only read the safety state.
2. **No remote reset of a Level 2/3 latch.** The dashboard may *acknowledge* alarms (silence the buzzer). Unlatching needs the physical Reset button (GPIO 35).
3. **ML and health scores are advisory.** They never open a relay. Trips come only from the deterministic, explainable 2-of-3 vote.
4. **Fail-secure stays the default.** Relays are active-HIGH (decision 41): boot, crash, watchdog reset and loss of 3.3 V all release them, bypassing every cell. A cell is energised into the string only after the power-up self-test passes.
5. **Dashboard:** password-protected Wi-Fi access point; read-only by default; config changes need the admin password and are logged.
6. **Never overclaim:**
   - gas readings are relative (not calibrated ppm);
   - 0.75 mV is resolution, not accuracy;
   - the health score is an indicator, not a certified state-of-health;
   - ML is a demonstration unless it's validated on real data.

## 1. Priority 1 — core smart features (build with the safety firmware)

| # | Feature | How it works on the existing hardware |
|---|---|---|
| P1.1 | **Live Wi-Fi dashboard** (phone or laptop) | ESP32 runs its own access point plus a web server, and pushes live data over WebSocket about once per second. Needs no internet and no app: open the page in a browser. Shows per-cell V, T, CO, H2, relay state and fault level; pack I, V, P; ambient T/RH; event list. Page files live in the ESP32 flash (LittleFS); chart library bundled, no CDN. |
| P1.2 | **Incident "black box"** | RAM ring buffer of all signals (about 2 min). On any Level 1/2/3 event, the pre- and post-event data plus an **incident report** (cell, signals in trip order, timestamps, isolation time) go to the microSD card with a DS3231 timestamp. Downloadable from the dashboard (CSV/JSON). |
| P1.3 | **Smart gas detection** | (a) Rolling per-sensor baseline with humidity/temperature correction from the BME280. (b) Rate-of-change detection as well as level. (c) **Common-mode rejection:** if the gas sensors of most cells rise together, it's room air (incense, cooking), not a cell, so it raises an "ambient gas" alert without counting toward the vote. A cell's gas signal counts only when its rise clearly exceeds the other cells'. (d) MQ warm-up period excluded from voting. |
| P1.4 | **Smart temperature and voltage signals** | Temperature: absolute limit plus rise above ambient (BME280) and above neighbouring cells, plus rate of rise. Voltage sag: the cell's deviation from the other in-string cells after subtracting the part explained by load current × the measured internal-resistance difference (P2.1). Bypassed cells are excluded from the voltage signal. |
| P1.5 | **Sensor self-diagnostics** | Detects a missing or disconnected DS18B20 (−127 °C / power-on 85 °C), MQ open or short (ADC near the rails), ADS1115/INA219/BME280 not answering on I2C, and stuck readings. A faulted sensor is shown on the OLED, LEDs and dashboard and never counts as "active". Open question for Aryan/Pranav (FMEA): should a faulted sensor make the remaining signals trip alone (more trips) or not (current default: not)? |
| P1.6 | **Relay verification** | After every relay command, check node voltages: an in-string cell reads about its cell voltage (node k − node k−1); a bypassed cell reads about 0 V. A mismatch means a stuck or welded relay, which is a Level 1 alert. The pack relay is checked through the INA219 bus voltage. |
| P1.7 | **Calibration wizard and settings** | Web page for the per-channel two-point voltage calibration (enter the multimeter readings) and for thresholds, the correlation window (60–120 s) and the heater cell. Stored in ESP32 flash (NVS). Every change is logged. |
| P1.9 | **Undervoltage load cutoff** | If any in-string cell falls below `cell_uv_v` (placeholder 3.0 V; the 25R cut-off is 2.5 V, to verify), the load switches off with an alert until the operator turns the load off and on again. Added 11 Nov after the twin showed over-discharge |
| P1.8 | **Power-up self-test** | Check every sensor, I2C address and relay (verified one at a time). Only then energise cells into the string. Result shown on the OLED and dashboard. |

## 2. Priority 2 — health and connectivity features

| # | Feature | How |
|---|---|---|
| P2.1 | **Per-cell internal resistance** ⭐ | Use the existing load MOSFET: switch the 10 Ω load on for about 1 s, then compute R = ΔV_cell / ΔI_pack per cell (ADS1115 + INA219). Run on demand from the dashboard and periodically when idle. Trend it per cell and compare cells. Expected ΔV: tens of mV per cell (estimate, check the Samsung 25R datasheet) against 0.75 mV ADC resolution. |
| P2.2 | **Charge, energy, state-of-charge** | Coulomb counting from the INA219 (mAh and Wh discharged), plus a state-of-charge estimate from rest voltage. Discharge only: cells are charged outside the rig (Lii-500). The OCV curve must come from the datasheet or our own measurement. |
| P2.3 | **Health score per cell (0–100)** | Rule-based, from internal-resistance rise, voltage imbalance, temperature rise under load and gas-baseline drift. Shown as a gauge. Labelled "indicator". |
| P2.4 | **Phone alerts** | When the access point or venue Wi-Fi has internet: Telegram bot or ntfy push ("Cell 3 isolated: gas + temperature at 14:32"). Without internet: alert banner plus buzzer on the dashboard. No personal data in the payloads. |
| P2.5 | **Test modes** | (a) Heater ramp control (set °C/min, hard stop at 55 °C, 50% duty cap, KSD301 70 °C backup). (b) Load-pulse profile on the load MOSFET that imitates a scooter ride. (c) Clearly labelled "simulate fault" mode that injects virtual signals to demo the vote; never used to trip real hardware unattended. |
| P2.6 | **Firmware update over Wi-Fi (OTA)** | Password protected; refused while any latch is active. |
| P2.7 | **Clock sync** | Network time sets the DS3231 when internet is available. |
| P2.8 | **Status UI** | OLED pages (pack, cells, events), 12 status LEDs (green normal, yellow Level 1, red Level 2/isolated), buzzer patterns per level, Acknowledge button. |

## 3. Priority 3 — stretch (only if time allows)

| # | Feature | Honest framing |
|---|---|---|
| P3.1 | ML anomaly detection | Train a small anomaly model on recorded normal operation (laptop), run it on the ESP32 (TFLite Micro / Edge Impulse). Output an anomaly score shown next to the vote; advisory only. |
| P3.2 | Health trend prediction | Trend each cell's internal resistance and capacity over time, fit on the laptop, show "projected trend". Not a validated remaining-life prediction: we have 4 cells and weeks of data, and public datasets (NASA, CALCE) use different cells. |
| P3.3 | Cloud logging | ThingSpeak or Firebase, optional. The demo must never depend on internet. |

## 4. Hardware limits to respect

- **Wi-Fi current peaks on the AMS1117 3.3 V regulator** (budget estimate ~0.5 A board peak): reduce Wi-Fi transmit power. A brownout bypasses every cell, which is safe but ruins a demo.
- **Pack current only (INA219):** all in-string cells carry the same current.
- **Free pins:** only GPIO39 (input-only), plus GPIO36 while JP2 is in its default position. No new sensors without hardware changes.
- **ESP32 ADC2 is unusable with Wi-Fi:** irrelevant, because all analog reads go through the external ADS1115s.

## 5. Simulation and test plan (before the hardware arrives)

| Level | Tool | Tests |
|---|---|---|
| 1. Logic tests | PlatformIO native + Unity (g++ on the laptop) | Safety core against scripted scenarios: every voting, window, latching, reset, fault and heater-cutoff case |
| 2. Digital twin (software-in-the-loop) | Python (`sim/`): battery model + thermal + vent gas + sensor models, with the **real C++ safety core** loaded as a shared library | Closed-loop runs: cell 3 runaway, incense in the room, two-cell failure, sensor fault, late second signal, heater test; plots for the deck; noisy repeated runs for false-alarm rate and detection time |
| 3. Firmware emulation | Wokwi (ESP32 + OLED + DS18B20 + relays + buttons + LEDs; potentiometers or mocks for ADS1115/INA219/MAX6675) | Tasks, Wi-Fi dashboard, UI |
| 4. Bare ESP32 | Real DevKitC with the "mock sensors" build flag | Wi-Fi, SD, OLED, watchdog, relay outputs before the rig exists |

- Assumed model values (cell parameters, vent-gas and runaway profiles) live in one config file. Each is labelled **ASSUMED** until it's replaced by a datasheet, a cited paper or a rig measurement.
- The simulation validates logic and timing against modelled faults. It does **not** prove detection on real cells.
- "Isolation < 1 s" must be measured on the hardware.

## 6. Build order (steps 1-3 done; step 4 written and compiling, waiting for modules)

1. Safety core + logic tests (Level 1).
2. Digital twin (Level 2).
3. ESP32 skeleton: FreeRTOS tasks, pin map, mock layer, watchdog.
4. Real sensor drivers as the modules arrive.
5. P1 features (dashboard, black box, diagnostics, relay verification, calibration, self-test).
6. P2 features.
7. P3 if time allows.
