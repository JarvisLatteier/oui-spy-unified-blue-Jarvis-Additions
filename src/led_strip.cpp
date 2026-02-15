/*
 * Unified LED Strip Implementation
 *
 * APA102 bit-bang for T-Dongle-S3, Adafruit_NeoPixel wrapper for XIAO.
 */

#include "led_strip.h"
#include "board_config.h"

// Current state
static uint8_t _r = 0, _g = 0, _b = 0;
static uint8_t _brightness = 31;  // APA102: 0-31, NeoPixel: 0-255

// ============================================================================
#if HAS_APA102
// APA102 bit-bang SPI (T-Dongle-S3)
// ============================================================================

static void apa102_sendByte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(APA102_DATA_PIN, (b >> i) & 1);
        digitalWrite(APA102_CLOCK_PIN, HIGH);
        digitalWrite(APA102_CLOCK_PIN, LOW);
    }
}

void ledStrip_init() {
    pinMode(APA102_DATA_PIN, OUTPUT);
    pinMode(APA102_CLOCK_PIN, OUTPUT);
    digitalWrite(APA102_DATA_PIN, LOW);
    digitalWrite(APA102_CLOCK_PIN, LOW);
    ledStrip_clear();
    ledStrip_show();
}

void ledStrip_setColor(uint8_t r, uint8_t g, uint8_t b) {
    _r = r; _g = g; _b = b;
}

void ledStrip_setBrightness(uint8_t brightness) {
    _brightness = brightness >> 3;  // Map 0-255 to 0-31
    if (_brightness < 1 && brightness > 0) _brightness = 1;
}

void ledStrip_show() {
    // Start frame: 32 bits of 0
    for (int i = 0; i < 4; i++) apa102_sendByte(0x00);
    // LED frame: 111 + 5-bit brightness, then B G R
    apa102_sendByte(0xE0 | (_brightness & 0x1F));
    apa102_sendByte(_b);
    apa102_sendByte(_g);
    apa102_sendByte(_r);
    // End frame: 32 bits of 1
    for (int i = 0; i < 4; i++) apa102_sendByte(0xFF);
}

void ledStrip_clear() {
    _r = 0; _g = 0; _b = 0;
}

// ============================================================================
#elif HAS_NEOPIXEL
// Adafruit NeoPixel wrapper (XIAO)
// ============================================================================

#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel _pixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

void ledStrip_init() {
    _pixel.begin();
    _pixel.setBrightness(50);
    _pixel.clear();
    _pixel.show();
}

void ledStrip_setColor(uint8_t r, uint8_t g, uint8_t b) {
    _pixel.setPixelColor(0, _pixel.Color(r, g, b));
}

void ledStrip_setBrightness(uint8_t brightness) {
    _pixel.setBrightness(brightness);
}

void ledStrip_show() {
    _pixel.show();
}

void ledStrip_clear() {
    _pixel.clear();
}

// ============================================================================
#else
// No LED strip — stubs
// ============================================================================

void ledStrip_init() {}
void ledStrip_setColor(uint8_t, uint8_t, uint8_t) {}
void ledStrip_setBrightness(uint8_t) {}
void ledStrip_show() {}
void ledStrip_clear() {}

#endif
