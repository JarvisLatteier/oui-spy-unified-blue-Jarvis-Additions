/*
 * Board Configuration — Pin Mapping & Feature Flags
 *
 * Supports:
 *   - Seeed XIAO ESP32-S3        (default)
 *   - LILYGO T-Dongle-S3         (BOARD_TDONGLE_S3=1 via build flag)
 *   - Freenove ESP32-S3 2.8" CYD (BOARD_CYD_S3=1 via build flag)
 *
 * Include this BEFORE any raw source file to override conflicting pin defines.
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#ifdef BOARD_TDONGLE_S3
// ============================================================================
// LILYGO T-Dongle-S3
// ============================================================================
// TFT uses GPIO3 (MOSI), GPIO4 (CS), GPIO5 (SCLK), GPIO2 (DC), GPIO1 (RST)
// SD_MMC uses GPIO21 (D2) among others
// No buzzer, no NeoPixel — has APA102 RGB LED instead

// Feature flags
#ifndef HAS_BUZZER
#define HAS_BUZZER      0
#endif
#ifndef HAS_NEOPIXEL
#define HAS_NEOPIXEL    0
#endif
#ifndef HAS_APA102
#define HAS_APA102      1
#endif
#ifndef HAS_SD_CARD
#define HAS_SD_CARD     1
#endif

// Pin assignments
#ifndef BUZZER_PIN
#define BUZZER_PIN      -1
#endif
#ifndef LED_PIN
#define LED_PIN         -1
#endif
#ifndef NEOPIXEL_PIN
#define NEOPIXEL_PIN    -1
#endif

// APA102 RGB LED
#define APA102_DATA_PIN   40
#define APA102_CLOCK_PIN  39

// LEDC channel allocation: ch0=backlight(display.cpp), ch0=buzzer (no conflict, buzzer disabled)
#define BUZZER_LEDC_CH  0

// TFT display (configured via TFT_eSPI build flags, listed here for reference)
// MOSI=3, SCLK=5, CS=4, DC=2, RST=1, BL=38

// SD_MMC pins
#define SD_MMC_CLK_PIN  12
#define SD_MMC_CMD_PIN  16
#define SD_MMC_D0_PIN   14
#define SD_MMC_D1_PIN   17
#define SD_MMC_D2_PIN   21
#define SD_MMC_D3_PIN   18

// Backlight
#define TFT_BL_PIN      38

#elif defined(BOARD_CYD_S3)
// ============================================================================
// Freenove ESP32-S3 2.8" IPS Capacitive Touch Display (FNK0104B)
// ============================================================================
// ILI9341 320x240 SPI display, FT6336U capacitive touch, SD_MMC 4-bit,
// WS2812 NeoPixel, GY-NEO6MV2 GPS on UART0

// Feature flags
#ifndef HAS_BUZZER
#define HAS_BUZZER      1    // Speaker via ES8311 codec I2S DOUT
#endif
#ifndef HAS_NEOPIXEL
#define HAS_NEOPIXEL    1    // WS2812 on GPIO 42
#endif
#ifndef HAS_APA102
#define HAS_APA102      0
#endif
#ifndef HAS_SD_CARD
#define HAS_SD_CARD     1    // SD_MMC 4-bit
#endif

// Pin assignments
#ifndef BUZZER_PIN
#define BUZZER_PIN      8    // I2S DOUT (ES8311 codec)
#endif
#ifndef LED_PIN
#define LED_PIN         -1
#endif
#ifndef NEOPIXEL_PIN
#define NEOPIXEL_PIN    42   // WS2812 RGB LED
#endif

// Speaker amplifier enable (active-LOW per Freenove examples: LOW=on, HIGH=off)
#define SPEAKER_EN_PIN  1

// LEDC channel allocation: ch0=backlight(display_cyd.cpp), ch1=buzzer
#define BUZZER_LEDC_CH  1

// SD_MMC pins (4-bit mode)
#define SD_MMC_CLK_PIN  38
#define SD_MMC_CMD_PIN  40
#define SD_MMC_D0_PIN   39
#define SD_MMC_D1_PIN   41
#define SD_MMC_D2_PIN   48
#define SD_MMC_D3_PIN   47

// Touch I2C (FT6336U)
#define TOUCH_SDA_PIN   16
#define TOUCH_SCL_PIN   15
#define TOUCH_RST_PIN   18
#define TOUCH_INT_PIN   17
#define TOUCH_I2C_ADDR  0x38

// GPS UART (UART0, free since USB CDC handles Serial)
#define GPS_RX_PIN      44
#define GPS_TX_PIN      43
#define GPS_BAUD        9600

// Backlight
#define TFT_BL_PIN      45

#else
// ============================================================================
// Seeed XIAO ESP32-S3 (default)
// ============================================================================

// Feature flags
#ifndef HAS_BUZZER
#define HAS_BUZZER      1
#endif
#ifndef HAS_NEOPIXEL
#define HAS_NEOPIXEL    1
#endif
#ifndef HAS_APA102
#define HAS_APA102      0
#endif
#ifndef HAS_SD_CARD
#define HAS_SD_CARD     0
#endif

// Pin assignments (match existing firmware defaults)
#ifndef BUZZER_PIN
#define BUZZER_PIN      3
#endif
#ifndef LED_PIN
#define LED_PIN         21
#endif
#ifndef NEOPIXEL_PIN
#define NEOPIXEL_PIN    4
#endif

// LEDC channel for buzzer (no TFT backlight on XIAO, so ch0 is free)
#define BUZZER_LEDC_CH  0

#endif // BOARD_TDONGLE_S3

#endif // BOARD_CONFIG_H
