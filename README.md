# VentSense — Multimodal Early Vent-Gas & Thermal-Runaway Cell Isolation

**Vishwakarma Awards 2026-27** · Sustainable Energy track (mentor: IIT Hyderabad) · Application ID `VK26-IITH-SE-0279`
Team VentSense, IIT (BHU) Varanasi

VentSense is a low-cost safety add-on for small Li-ion packs. Every cell segment is watched by three dissimilar signals: vent gas (MQ-7 for CO, MQ-8 for H2), cell-wall temperature (DS18B20) and voltage sag relative to the neighbouring cells (ADS1115). An ESP32 runs 2-of-3 voting. When two signal types trip on the same cell within a 60–120 s window, that cell's fail-secure SPDT bypass relay drops and the other cells keep powering the load. If two or more cells trip, the pack relay opens.

Our claim is narrow: **2-of-3 voting across dissimilar signals plus per-cell isolation, as a low-cost retrofit for small packs.** Gas-based and voltage-based battery monitoring both exist already as prior art.

## Repository map

| What | Status | Path |
| :--- | :--- | :--- |
| KiCad 10 project (6 hierarchical sheets, 134 symbols) | ERC: 0 errors, 0 warnings (default checks) | [`kicad/VentSense_Stage2/`](kicad/VentSense_Stage2/) |
| **Controller carrier PCB v1.1 (for fabrication)** | DRC: 0 violations, 0 unconnected, 0 parity issues (default checks, `--schematic-parity`) | [`PCB/`](PCB/) (ordering guide: [`PCB/ORDERING.md`](PCB/ORDERING.md)) |
| Gerber + Excellon package v1.1 | 2 layers, 124 × 126 mm | [`PCB/fabrication/VentSense_Carrier_Gerbers_v1.1.zip`](PCB/fabrication/VentSense_Carrier_Gerbers_v1.1.zip) |
| Stage 2 exports (schematic PDF/PNG, ERC/DRC, BOM CSV) | as submitted at Stage 2 (Oct 2026) | [`exports/`](exports/) |
| Bill of materials | Rev 2.1 Final | [`bom/`](bom/) |
| Mechanical CAD (SolidWorks) and wax thermal study | | [`ventsense_cad/`](ventsense_cad/) |
| Superseded v1.0 PCB outputs | archived, do not order | [`PCB/archive_v1.0/`](PCB/archive_v1.0/) |

## Schematic sheets

1. **Power:** 12 V input, IRF4905 reverse-polarity protection, heater branch (5 A fuse, E-stop NC2, KSD301 70 °C cutoff), TC4427 gate driver, IRLZ44N switches.
2. **Pack_Path:** 4S INR18650-25R string, 4 SPDT bypass relays, pack relay (COM→NO), E-stop NC1, 3 A fuse, INA219, XT60, 10 Ω load.
3. **Measurement:** 100 k/20 k cell dividers into ADS1115 #3 (0x4B); 15 k/22 k gas dividers into ADS1115 #1 (0x48, CO) and #2 (0x49, H2); DS18B20 1-Wire bus.
4. **MCU_IO:** ESP32-DevKitC-32E, OLED, BME280, DS3231 RTC, microSD, MAX6675, 2 × PCF8574 status LEDs, buzzer, buttons.
5. **Connectors:** the carrier board's off-board connectors and solder jumpers (added in v1.1).

## Controller carrier PCB v1.1

- 2-layer FR-4, 1.6 mm, 1 oz copper. 124 × 126 mm, 4 × M3 holes. Minimum track and gap 0.2 mm, minimum drill 0.4 mm (vias), via annular ring 0.2 mm.
- Logic and signal currents only. Heater, load and pack currents stay off this board.
- Screw terminals (5.08 mm): `+12V`, gate outputs to the off-board IRLZ44Ns, cell taps (≤ 16.8 V, kept on their own connector), and `+5V` from the XL4015, with 100 µF + 100 nF at the input.
- Headers for the 4-channel relay module, the pack relay module and the INA219.
- Plug-in sockets for the ESP32-DevKitC (rows 25.40 mm apart, per Espressif's DevKitC V4 drawing) and the 3 ADS1115 modules. The other modules use labelled sockets wired with jumper leads.
- Net-class track widths: `+5V` 1.0 mm, `+3V3`/`GND`/`RLY_VCC` 0.5 mm, `+12V` 0.4 mm (TC4427 supply only), signals 0.25 mm. GND pours on both layers, stitched with vias.
- **JP1 (relay module VCC):** 1-2 = `+5V` (default), 2-3 = `+3V3`.
- **JP2 (MAX6675 SO):** 1-2 = shared SPI MISO (IO19, default), 2-3 = IO36. This is the fallback if the microSD module doesn't release MISO.

All PCB files (Gerbers, drill files, BOM, reports, renders) and the ordering note are in [`PCB/`](PCB/).
