// ESP32-DevKitC-32E pin map: source of truth is design spec section 5.4 / 5.5.
#pragma once

// I2C bus (ADS1115 x3, INA219, BME280, DS3231, SSD1306, PCF8574 x2)
#define PIN_SDA 21
#define PIN_SCL 22
// 1-Wire (4 x DS18B20, 4.7k pull-up R33 on the carrier)
#define PIN_ONEWIRE 4
// SPI: microSD + MAX6675 share SCK/MISO
#define PIN_SPI_SCK 18
#define PIN_SPI_MISO 19
#define PIN_SPI_MOSI 23
#define PIN_SD_CS 5
#define PIN_TC_CS 14
#define PIN_TC_SO_ALT 36  // only if JP2 is moved to 2-3 (MAX6675 SO -> IO36)

// Relays: active-HIGH (decision 41, H trigger). HIGH = energised = cell in string.
// High-Z at reset (no pull), so the relays stay released during boot.
#define PIN_RLY_SEG1 25
#define PIN_RLY_SEG2 26
#define PIN_RLY_SEG3 27
#define PIN_RLY_SEG4 32
#define PIN_RLY_PACK 33

// TC4427 gate-driver inputs (100k pull-downs R2/R3 on the carrier)
#define PIN_HTR_EN 16
#define PIN_LOAD_EN 17

// UI
#define PIN_BUZZER 13   // BC547 low side, 5 V active buzzer
#define PIN_BTN_ACK 34  // input-only, external 10k pull-up, pressed = LOW
#define PIN_BTN_RST 35  // input-only, external 10k pull-up, pressed = LOW

// I2C addresses (design spec 5.5)
#define I2C_ADS_CO 0x48    // ADS1115 #1, ADDR -> GND, MQ-7 x4
#define I2C_ADS_H2 0x49    // ADS1115 #2, ADDR -> VDD, MQ-8 x4
#define I2C_ADS_CELL 0x4B  // ADS1115 #3, ADDR -> SCL, cell taps (divider 100k/20k = 1/6)
#define I2C_INA219 0x40
#define I2C_BME280 0x76
#define I2C_DS3231 0x68
#define I2C_OLED 0x3C
#define I2C_PCF_1 0x20  // LEDs segments 1-2
#define I2C_PCF_2 0x21  // LEDs segments 3-4

static const int PIN_RLY_SEG[4] = {PIN_RLY_SEG1, PIN_RLY_SEG2, PIN_RLY_SEG3, PIN_RLY_SEG4};
