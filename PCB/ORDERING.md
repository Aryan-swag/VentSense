# Ordering the VentSense Controller Carrier PCB (v1.1)

This note is for whoever places the PCB order: the team, the Gymkhana purchase office, or the fab's support desk.

## 1. File to upload

**[`fabrication/VentSense_Carrier_Gerbers_v1.1.zip`](fabrication/VentSense_Carrier_Gerbers_v1.1.zip)**

Upload this zip as it is. Don't upload the KiCad project files or the PNG renders. The zip holds RS-274X Gerbers (Protel file extensions) and Excellon drill files in mm:

| File | Layer |
| --- | --- |
| `VentSense_Stage2-F_Cu.gtl` | Top copper |
| `VentSense_Stage2-B_Cu.gbl` | Bottom copper |
| `VentSense_Stage2-F_Mask.gts` / `-B_Mask.gbs` | Top / bottom solder mask |
| `VentSense_Stage2-F_Silkscreen.gto` / `-B_Silkscreen.gbo` | Top / bottom silkscreen |
| `VentSense_Stage2-Edge_Cuts.gm1` | Board outline |
| `VentSense_Stage2-PTH.drl` | Plated holes (506) |
| `VentSense_Stage2-NPTH.drl` | Non-plated holes (4 × 3.2 mm, M3 mounting) |
| `VentSense_Stage2-job.gbrjob` | Gerber job file (stack-up description) |

## 2. Order options

| Option | Value |
| --- | --- |
| Layers | 2 |
| Board size | **124.0 × 126.0 mm** (single board, no panel) |
| Quantity | 5 |
| Material | FR-4, standard Tg |
| Thickness | 1.6 mm |
| Copper weight | 1 oz (35 µm), outer layers |
| Surface finish | HASL (leaded or lead-free; either is fine) |
| Solder mask | Green, both sides |
| Silkscreen | White, both sides (the back carries jumper notes) |
| Minimum track / spacing | 0.25 mm track, 0.20 mm spacing |
| Minimum drill | 0.40 mm (vias); smallest component hole 0.75 mm |
| Minimum annular ring | 0.15 mm |
| Vias | 0.8 mm pad / 0.4 mm drill, tented (covered by mask), as in the Gerbers |
| Electrical test | Yes, if offered (flying probe) |
| Not needed | Stencil, assembly, impedance control, castellated holes, gold fingers, edge plating |
| Fab order number | If the fab prints one, ask for it on the **back**; the front is full |

All parts are through-hole. The two solder jumpers JP1 and JP2 are bridged by copper in their default position, so the board needs no paste or stencil.

## 3. Cost

- The purchase list (line 48) budgets **₹800 for 5 boards (estimate)**, for "about 124 × 112 mm".
- The final board is **124 × 126 mm**, so get the real quote from the fab's online calculator (e.g. LionCircuits, PCBPower) before you confirm the order.
- If the quote is far above the budget, tell the team first. Perfboard and an ESP32 screw-terminal breakout are already on the order as a fallback.

## 4. Check in the fab's online Gerber viewer before you pay

- [ ] The size it reads is 124 × 126 mm, 2 layers.
- [ ] The outline is a plain rectangle with 4 corner mounting holes.
- [ ] The top silkscreen reads normally, with "VentSense Controller v1.1" along the bottom edge. The back text appears mirrored when viewed from the top.
- [ ] Copper shows on both layers, with ground fill on both sides.
- [ ] The fab's own DFM check passes. If it raises anything, send the message to the team (Aryan) before you accept any change.

Design checks done in KiCad 10.0.6 with all default rules on: ERC 0 errors / 0 warnings; DRC 0 violations, 0 unconnected pads, 0 schematic-parity issues. The reports are in [`docs/`](docs/).

## 5. When the boards arrive (team)

**Bare-board test before soldering.** With a multimeter, confirm there is no short between:
- `+5V` and `GND` (J3 terminals);
- `+12V` and `GND` (J4 terminals);
- `3V3` and `GND` (J9 header pins 1 and 2).

**Solder in this order, lowest parts first:**
1. Resistors.
2. Ceramic capacitors. Fit the four 100 nF capacitors next to the ADS1115 sockets flat, because the ADS1115 modules sit over them.
3. DIP-8 socket.
4. BC547.
5. Switches.
6. LEDs.
7. JST-XH connectors.
8. Headers and sockets.
9. Screw terminals.
10. 100 µF capacitor.
11. Buzzer.

**Polarity:**

| Part | Rule |
| --- | --- |
| LEDs | Square pad = cathode (short leg, flat side) |
| C11 100 µF | `+` lead to the marked + side |
| Buzzer | `+` pin to the pad marked + |
| TC4427 | Notch / pin 1 to the square pad |
| BC547 | Flat face as drawn on the silkscreen |

**Leave out:**
- **Don't fit R34 / R35** (I2C pull-ups) unless the modules turn out to have none of their own.
- **Leave JP1 and JP2 as delivered** (1-2 bridged):
  - JP1 1-2 = relay module VCC on +5 V.
  - JP2 1-2 = MAX6675 SO on the shared SPI MISO (IO19).
  - To change one, cut the thin copper link between pads 1 and 2, then bridge pads 2 and 3 with solder.

**First power-up:**
1. Set the XL4015 output to 5.0 V with nothing connected.
2. Power the board with only the ESP32 plugged in.
3. Check 5 V and 3.3 V.
4. Then plug in the ADS1115s and the other modules, one at a time.

**Module checks on arrival:**
- **OLED:** pin order GND-VCC-SCL-SDA.
- **BME280:** check the chip marking; many listings are BMP280.
- **DS3231:** remove the coin-cell charging resistor (the CR2032 is not rechargeable).
- **PCF8574:** set the two modules to 0x20 and 0x21.
- **Green LEDs:** must be low-Vf (2.2 V or less).
- **microSD module:** with its CS high, MISO must float. If it doesn't, move JP2 to 2-3.
- **Relay modules:** bench-test the trigger jumper setting with a 3.3 V signal before connecting cells. The board works with either setting.
