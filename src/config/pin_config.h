#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

// ── Boot-essential hardware only ──────────────────────────
// Display pins, sensors, etc. are NOT here — those come from
// /etc/hardware.conf on SD and are read by loadable modules.

// SD card SPI bus (needed to boot — loads everything else)
#define PIN_SD_MOSI     38
#define PIN_SD_CLK      39
#define PIN_SD_MISO     40
#define PIN_SD_CS       46
#define SD_SPI_HOST     SPI3_HOST

// I2C (CardKB for shell input at boot)
#define PIN_I2C_SDA     8
#define PIN_I2C_SCL     9
#define CARDKB_ADDR     0x5F

// Buzzer
#define PIN_BUZZER      4

#endif
