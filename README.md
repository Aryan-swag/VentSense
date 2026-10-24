# VentSense — Multimodal Early Vent-Gas & Thermal Runaway Isolation System
**Vishwakarma Awards 2026-27 | Stage 2: Design Specification Repository**  
**Application Unique ID:** `VK26-IITH-SE-0279`

---

## 1. Executive Summary & Core Innovation
**VentSense** is a multimodal battery safety and per-cell fault-isolation system for 4S1P INR18650-25R lithium-ion packs. It correlates early electrolyte vent-gas signatures (CO via MQ-7, H2 via MQ-8), per-cell wall temperature (DS18B20 1-Wire array), and calibrated cell voltages (ADS1115 16-bit I2C ADCs, 0.75 mV resolution) within a 60-120 s sliding voting window. Upon confirming a dual-modality fault, the ESP32 controller de-energizes a dedicated fail-secure SPDT relay in < 1.0 s.

## 2. Stage 2 Audit Quick-Links (For Jury Verification)

| Deliverable / Artifact | Verification Metric / Status | Repository Path |
| :--- | :--- | :--- |
| **Native KiCad 10 Schematics (5 Sheets)** | 123 symbols, 100% footprint-mapped | [`kicad/VentSense_Stage2/`](kicad/VentSense_Stage2/) |
| **Electrical Rules Check (ERC) Log** | **0 Errors, 0 Warnings** | [`exports/VS_ERC.rpt`](exports/VS_ERC.rpt) |
| **Full Schematic PDF & PNG Renders** | High-res PDF and per-sheet PNGs | [`exports/VS_schematic.pdf`](exports/VS_schematic.pdf) |
| **2-Layer Controller Carrier PCB Layout** | 124 x 112 mm FR4 carrier (93 on-board parts) | [`kicad/VentSense_Stage2/VentSense_Stage2.kicad_pcb`](kicad/VentSense_Stage2/VentSense_Stage2.kicad_pcb) |
| **Design Rules Check (DRC) Log** | **0 DRC Violations, 0 Unconnected Pads** | [`exports/VS_DRC.rpt`](exports/VS_DRC.rpt) |
| **Auto-Routed Production PCB & Gerbers** | **899 routed copper segments, 20 vias** | [`hardware/pcb_production/VentSense_Carrier_Gerbers_v1.0.zip`](hardware/pcb_production/VentSense_Carrier_Gerbers_v1.0.zip) |
| **Unpacked Gerber & Drill Layers** | RS-274X Gerbers + Excellon PTH/NPTH drills | [`hardware/pcb_production/gerbers/`](hardware/pcb_production/gerbers/) |
| **Itemized Bill of Materials (BOM)** | 73 Core lines + 25 Spares (100% domestic) | [`bom/`](bom/) |

## 3. Five-Sheet Hierarchical Architecture
- **Sheet 1 (`VentSense_Stage2.kicad_sch`):** Top-level interconnect for power rails (+12V, +5V, +3V3, GND), I2C, SPI, 1-Wire, and relay control.
- **Sheet 2 (`Power_Protection.kicad_sch`):** 12V 5A input, IRF4905 reverse-polarity protection, 5A/3A fuses, 2-NC E-Stop, XL4015 5A buck, TC4427 gate driver, IRLZ44N switches, and KSD301 thermal cutoff.
- **Sheet 3 (`Battery_Switching.kicad_sch`):** 4S1P INR18650-25R string, 4x SPDT fail-secure bypass relays, pack disconnect relay, Bourns MF-R250 PTCs, and 100k/20k 1% dividers.
- **Sheet 4 (`Sensing_Array.kicad_sch`):** 3x ADS1115 16-bit ADCs (0x48, 0x49, 0x4B), INA219 current monitor (0x40), 4x DS18B20 probes, BME280 (0x76), and MAX6675 thermocouple verifier.
- **Sheet 5 (`MCU_And_UI.kicad_sch`):** ESP32-DevKitC-32E, DS3231 RTC (0x68), SSD1306 OLED (0x3C), 2x PCF8574 LED expanders (0x20, 0x21), BC547 buzzer driver, and microSD SPI logger.
