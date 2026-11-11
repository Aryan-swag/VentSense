# VentSense digital twin (software-in-the-loop)

A Python model of the rig with the **real firmware safety core** in the loop. `vs_core.py` compiles `../firmware/lib/safety_core` into `build/libvscore.so` and calls it through ctypes. The ESP32 runs exactly the same voting code.

## What is modelled

| Part | Model |
|---|---|
| Cells | 4 × INR18650-25R: open-circuit voltage curve, internal resistance + one RC pair, charge counting, internal short for a failing cell |
| Pack path | Bypass relays (node arithmetic for the taps), pack relay, 10 Ω load, shunt and path resistance |
| Thermal | Lumped cells with neighbour coupling and wax-jacket latent heat at 58–60 °C; heater block, dummy cell and KSD301 for heater tests |
| Gas | Vent-gas pocket per cell, room air (incense), local injection (electrolysis H2), MQ first-order response, 15k/22k divider |
| Sensors | ADS1115 quantisation, calibrated tap gain error, DS18B20 750 ms update + 0.0625 °C steps + offset, INA219 noise/LSB, BME280 |

Every number is in `params.py` and tagged SPEC, DATASHEET or **ASSUMED**. Replace the ASSUMED values with datasheet, literature or rig measurements as they become available.

## Run

```
python3 run_all.py            # 9 scenarios + 40+40 Monte Carlo runs, about 90 s
python3 run_all.py --mc 0     # scenarios only
```

Outputs:
- `out/summary.md`: every check (pass/fail) and the Monte Carlo statistics;
- `out/<scenario>.png`: timeline plots (gas, temperature, cell voltage, pack current, level), for slides.

## Dashboard on the laptop

```
python3 dash_server.py --speed 5     # http://localhost:8080, admin password ventsense-admin
```

This serves the real dashboard (`../firmware/web/index.html`) and the ESP32 API (`../firmware/API.md`) from a live twin. In the Console box you can type `fault 3` (failing cell), `room` (incense), `h2 1` (hydrogen into a pocket), `irtest`, `ir baseline`, `ack`, or `reset` (the simulated physical button).

## Scenarios (`scenarios.py`)

| Name | What it shows |
|---|---|
| `normal` | Discharge with a load on/off cycle: no alerts |
| `incense_room` | All gas sensors rise together: ambient alert, no trip (common-mode rejection) |
| `cell3_runaway` | Gas, then heat and voltage sag on cell 3: only cell 3 isolated; the pack keeps powering the load; latency after the 2nd signal |
| `two_cell` | Cells 2 and 3 fail: Level 3 opens the pack relay |
| `sensor_unplug` | DS18B20 removed: sensor-fault alert, no trip |
| `heater_only` | Heater on a dummy cell: 55 °C firmware cutoff, 50 % duty cap, KSD301 backup |
| `rig_vote` | Planned rig demo: H2 into the dummy pocket, then heater: 2-of-3 vote |
| `late_second` | H2 pulse, then heat after the window expired: no vote |
| `ir_health` | Firmware IR test per cell (DCIR at 1 s) before and after synthetic ageing of cell 2; health indicator; bar chart `out/ir_health_bars.png` |

## Limits

- **Fault profiles are synthetic.** The simulation shows the firmware logic and timing behave as designed against these models. It does not prove detection on real cells.
- **Detection times depend on the ASSUMED fault profile.**
- **"Isolation < 1 s" must still be measured on the hardware.** In the model it's debounce 0.5 s + one 100 ms step.
