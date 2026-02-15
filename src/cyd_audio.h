/*
 * CYD Audio — I2S tone generator for Freenove ESP32-S3 2.8" CYD
 *
 * Drives the ES8311 codec via I2S to produce sine wave tones.
 * Drop-in replacement for Arduino tone()/noTone() which don't work
 * through the codec (it needs proper I2S audio data, not PWM).
 */

#pragma once

#ifdef BOARD_CYD_S3

#include <Arduino.h>

/* Initialize I2S + ES8311 codec + background tone task.
 * Safe to call multiple times (uses static init guard). */
void cyd_audio_init();

/* Start a tone (non-blocking, returns immediately).
 * pin is ignored (always uses I2S DOUT). duration in ms, 0 = indefinite. */
void cyd_tone(uint8_t pin, unsigned int frequency, unsigned long duration = 0);

/* Stop any active tone. pin is ignored. */
void cyd_noTone(uint8_t pin);

/* Set tone amplitude. Maps 0-255 to sine wave scaling factor. */
void cyd_set_volume(uint8_t vol_0_255);

#endif /* BOARD_CYD_S3 */
