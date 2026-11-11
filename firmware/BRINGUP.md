# Firmware bring-up on the real hardware

Follow these in order. Do each step with **no cells connected** until step 6. Serial console: `pio device monitor -b 115200` (or the VS Code PlatformIO monitor). Type a command and press Enter.

## 1. Bare ESP32: mock build
0. **Fit pull-ups first.** On a bare DevKitC, fit 10 kΩ from GPIO 34 to 3V3 and from GPIO 35 to 3V3. These are the button inputs; they have no internal pull-ups, and on the carrier R49/R50 do this job. Without them the Reset/Ack inputs float and can trigger at random.
1. Flash the mock build: `pio run -e esp32dev_mock -t upload -t monitor`. The boot message says `SELF-TEST PASSED (MOCK ...)`.
2. Join Wi-Fi `VentSense` on a phone and open `http://192.168.4.1`. You should see a live table of the 4 cells.
3. Type `gas 3`, then `temp 3`: cell 3 goes to Level 2 and GPIO 27 goes LOW. Type `clear`. The cell stays latched; check that only the GPIO 35 button resets it (jumper GPIO 35 to GND for a moment on the bare board).
4. Pass: the voting works on real silicon.

## 2. Carrier board without modules: real build
1. Flash `pio run -e esp32dev -t upload -t monitor`.
2. The self-test must say `FAILED: relays stay released`, with every module MISSING.
3. With a multimeter, check GPIO 25, 26, 27, 32 and 33 are LOW. That's the fail-secure state.

## 3. I2C modules, one at a time (power off while plugging)
Plug in each module, then type `selftest`:

| Module | Expected | If it fails |
|---|---|---|
| ADS1115 CO (ADDR→GND) | `0x48 OK` | ADDR wiring |
| ADS1115 H2 (ADDR→VDD) | `0x49 OK` | ADDR wiring |
| ADS1115 cells (ADDR→SCL) | `0x4B OK` | ADDR wiring |
| INA219 | `0x40 OK` | |
| BME280 | `0x76 OK` | `missing or BMP280` means the chip is probably a BMP280; check the marking |
| DS3231 | `OK` or `time not set` | Remove the charging resistor (CR2032) |
| OLED | `OK` | Some modules are 0x3D: change `I2C_OLED` in `include/pins.h` |
| PCF8574 ×2 | `0x20 OK 0x21 OK` | Set the A0–A2 jumpers on the modules. A PCF8574**A** sits at 0x38–0x3F: never 0x3C, which is the OLED |

**I2C pull-ups:** count the pull-up resistors on all modules. The parallel total must stay above about 1 kΩ. Remove extras and don't fit R34/R35.

## 4. DS18B20 probe-to-cell mapping (required for arming)
1. Type `probes`. It lists every probe with its ROM code and temperature.
2. Hold one probe in your hand for 20 s and type `probes` again. The warm one is that probe's `N`.
3. Assign it to its cell with `probe K N` (for example, `probe 1 2`). Repeat for all 4 cells. After the 4th, the mapping is saved to flash.
4. Type `selftest`. It should report `mapping confirmed`.

## 5. Tap calibration (two points per cell, before relying on voltages)
1. Connect a bench supply or a cell to one tap. Measure the CELLk+ terminal to pack negative with the multimeter.
2. Type `calpt K <meter volts>`, for example `calpt 1 3.702`.
3. Repeat at a second voltage at least 0.5 V different. One point gives a gain only; two points give gain + offset.
4. `calshow` lists all four; `calclear K` resets one.
5. Re-check against the multimeter. Note: 0.75 mV is resolution, not accuracy.

## 6. Relay modules (H trigger, decision 41)
1. Set the trigger jumper on both relay modules to **H**. Keep JP1 at 1-2 (+5 V).
2. Bench-test each input before wiring cells: IN floating → released, IN = 3.3 V → energised, IN = 0 V → released.
3. With the self-test passing and the system armed (`arm`), all relays should click in. Then `disarm` → all should release.
4. Watch for the `RELAY_MISMATCH` alert (event type 8, detail 2) once cells are connected. It means a relay isn't doing what it's told.

## 7. Remaining checks
- **INA219 sign:** discharge must read **positive**. If it's negative, swap VIN+/VIN− (pack side goes to VIN+).
- **microSD + MAX6675 sharing MISO:** with the SD card CS high, MISO must float. If the thermocouple reading is garbage (`tc` null or stuck), move JP2 to 2-3 and rebuild with `-DVS_TC_ON_IO36=1`.
- **Heater test** (dummy cell in segment 1):
  1. Set `segment_enabled[0] = 0` and `heater_cell = 0` in the config (for now, in `vs_default_config`).
  2. Type `heater on` and check the cutoff at 55 °C on the probe.
  3. Measure how much hotter the heater block runs than the dummy (the twin predicts a large gap; use thermal paste).
- **Timing:** measure "isolation < 1 s" on the hardware. Scope or log the GPIO from the second threshold crossing to relay release.

## Log format (microSD, `/vs_YYYYMMDD.csv`)

```
D,<rtc time>,<uptime s>,<level>,<alerts>,<pack V>,<pack A>,<thermocouple C>, then per cell: V, T, CO volts, H2 volts, relay, level
E,<rtc time>,<uptime s>,<event type>,<cell>,<detail>
```

Event types: 1 signal on, 2 signal off, 3 Level 2 trip, 4 Level 3 trip, 5 reset, 6/7 fault on/off, 8/9 alert on/off, 10/11 armed/disarmed, 12 ack. Signal detail: 0 gas, 1 temperature, 2 voltage.
