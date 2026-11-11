# VentSense HTTP API (ESP32 access point, `http://192.168.4.1`)

The ESP32 (`src/main.cpp`) and the laptop mock server (`sim/dash_server.py`, driven by the digital twin) implement the same API, so the dashboard (`web/index.html`) can be tested without hardware.

**Admin:**
- Endpoints marked 🔒 need the admin password, sent as the header `X-VS-Key` or the query parameter `key`.
- The default comes from the build flag `VS_ADMIN_PASS`. Change it with the console command `admin <new>`; it's stored in flash.
- No endpoint can reset a Level 2/3 latch. Only the physical button on GPIO 35 does that (safety rule 2).

## GET `/api/state` (once a second)

```json
{"fw":"1.1","mock":0,"t":123.4,"time":"2026-11-12 14:32:05","level":0,"armed":1,"selftest":1,
 "relaycheck":{"state":2,"phase":4,"code":0,"cell":-1,"meas":0,"exp":0},
 "sim":0,"alerts":0,"buzzer":0,
 "pack":{"relay":1,"v":14.80,"i":1.480,"w":21.9,"ah":0.120,"wh":1.80,"latched":0},
 "amb":{"t":27.0,"rh":50.0,"ok":1},"tc":28.25,
 "heater":{"on":0,"req":0,"duty":0.00,"mode":"off"},"load":{"on":1,"req":1,"profile":0},
 "irtest":{"state":0,"reason":0,"di":0.0},
 "cells":[{"v":3.700,"t":28.1,"co":0.01,"h2":0.00,"relay":1,"level":0,"latched":0,"faults":0,"sig":[0,0,0],
           "trip_t":-1,"order":[255,255,255],"soc":0.80,"ah":0.10,"score":98,"ir":0.0225,"ir_base":0.0224,"en":1}]}
```

| Field | Meaning |
|---|---|
| `level` | 0 normal, 1 alert, 2 a cell isolated, 3 pack relay open |
| `alerts` | bits: 1 room gas, 2 relay mismatch, 4 INA219 fault, 8 BME280 fault, 16 heater cutoff, 32 heater duty cap, 64 not armed, 128 sensor fault, 256 cell undervoltage (load switched off) |
| `relaycheck.state` | 0 idle, 1 running, 2 pass, 3 fail. `code`: 1 output live with all cells bypassed, 2 cell missing, 3 relay not switching / pack path open, 4 all-on mismatch, 5 no bus voltage |
| `cells[].faults` | bits: 1 CO, 2 H2, 4 temperature, 8 tap |
| `cells[].sig` | active signals [gas, temperature, voltage] |
| `cells[].order` | signal types in crossing order at the trip (255 = none) |
| `score` | health indicator 0–100 (advisory); `null` when not enough data |
| `irtest.state` | 0 idle, 1 pre, 2 step, 3 done, 4 aborted. `reason`: 1 alarm, 2 relays changed, 3 no current change, 4 no current reading, 5 no cell in string |

## GET `/api/history?since=<t>`

Up to 600 samples at 1 Hz (10 minutes), newer than `since`.

```json
{"cols":["t","lvl","rel","v1","v2","v3","v4","T1","T2","T3","T4","g1","g2","g3","g4","i","bus"],"rows":[[...],...]}
```

- `rel`: bits 0–3 = segment relays, bit 4 = pack relay.
- `g`: gas rise, the larger of MQ-7 / MQ-8, as a fraction of baseline.

## GET `/api/events?since=<id>`

```json
{"next":42,"events":[{"id":41,"t":443.2,"time":"...","type":3,"cell":2,"detail":3}]}
```

Event types are the ones in `safety_core.h` (`VS_EV_*`). There is one extra, type 100, for app notes such as "IR test done" or "relay check failed". Its `detail` is a code and `text` explains it.

## Incidents

- **GET `/api/incidents`** returns `{"sd":1,"last":{...},"items":[{"name":"inc_001.csv","size":12345}]}`.
- **GET `/api/file?name=inc_001.csv`** downloads one incident file: the 10 minutes before and 2 minutes after a Level 2/3 trip, 1 Hz.

## Settings

- **GET `/api/config`** returns `{"settings":{"window_s":{"v":90,"min":60,"max":120,"unit":"s","label":"..."},...}}`.
- 🔒 **POST `/api/config`** with form fields `<key>=<value>` sets settings. Values are range-checked and stored in flash, then applied between two safety steps. The reply is `{"ok":1,"err":""}`.
- Limits come from the spec and can only be tightened: heater cutoff ≤ 55 °C, duty ≤ 50%, window within 60–120 s.

## Commands: 🔒 GET `/api/cmd?c=<command>` (plain-text reply; the serial console accepts the same commands)

| Command | Action |
|---|---|
| `arm` / `disarm` | Operator arm; arming runs the relay check first |
| `ack` | Silence the buzzer (does not unlatch) |
| `relaycheck` | Run the power-up relay check again |
| `selftest` | Re-check all modules |
| `irtest` | Measure internal resistance (only at Level 0) |
| `ir baseline` | Store the latest IR result as the commissioning baseline (also used for sag compensation) |
| `load on` / `load off` / `profile 0` / `profile 1` | Load control; profile 1 = scooter-like pattern |
| `ramp <C/min> <target C>` / `heater off` | Heater ramp test (target ≤ cutoff − 2 °C); the core still enforces 55 °C and 50% |
| `sim gas K` / `sim temp K` / `sim off` | **Demo injection** of a virtual signal on cell K for up to 120 s. It's flagged `sim:1` and logged; the trip is real |
| `wifi <ssid> <password>` / `wifi off` | Join a network (for clock sync and phone alerts) |
| `ntfy <topic>` / `ntfy off` | Phone alerts via ntfy.sh on Level 2/3 and sensor faults (no personal data in messages) |
| `admin <new password>` | Change the admin password |
| real build: `probes`, `probe K N`, `calpt K V`, `calshow`, `calclear K` | Probe mapping and tap calibration (`BRINGUP.md`) |
| mock build: `gas K`, `temp K`, `sag K`, `unplug K`, `room`, `clear` | Simulated sensor faults |

## Self-test and updates

- **GET `/api/selftest`** returns `{"ok":1,"report":"SELF-TEST PASSED ..."}`.
- 🔒 **POST `/update?key=...`** is a firmware update (multipart `.bin` upload). It's refused while any cell is latched or the level is 2 or above. The board reboots afterwards, and every relay releases during the reboot.
