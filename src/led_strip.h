/*
 * Unified LED Strip Interface
 *
 * XIAO:      wraps Adafruit_NeoPixel (GPIO4, single pixel)
 * T-Dongle:  bit-bang APA102 protocol (GPIO40 data, GPIO39 clock)
 */

#ifndef LED_STRIP_H
#define LED_STRIP_H

#include <Arduino.h>
#include "board_config.h"

void ledStrip_init();
void ledStrip_setColor(uint8_t r, uint8_t g, uint8_t b);
void ledStrip_setBrightness(uint8_t brightness);
void ledStrip_show();
void ledStrip_clear();

#endif // LED_STRIP_H
