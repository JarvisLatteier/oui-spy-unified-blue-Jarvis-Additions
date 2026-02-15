/*
 * ES8311 Audio Codec Driver — adapted from Freenove ESP32-S3 Display example
 * SPDX-License-Identifier: Apache-2.0
 *
 * Only compiled for BOARD_CYD_S3 (Freenove 2.8" CYD with ES8311 + NS4150 amp).
 * Uses Wire (I2C) to configure the codec; I2S data path is handled separately.
 */

#pragma once

#ifdef BOARD_CYD_S3

#include "esp_types.h"
#include "esp_err.h"

/* ES8311 I2C address: CE pin low = 0x18 */
#define ES8311_ADDRESS  0x18

#define ES8311_SAMPLE_RATE     16000
#define ES8311_MCLK_MULTIPLE   256
#define ES8311_MCLK_FREQ_HZ   (ES8311_SAMPLE_RATE * ES8311_MCLK_MULTIPLE)
#define ES8311_VOICE_VOLUME    85

#ifdef __cplusplus
extern "C" {
#endif

/* Full codec initialization (reset, clock, format, power-up DAC) */
esp_err_t es8311_codec_init(void);

/* Mute/unmute DAC output */
void es8311_set_voice_mute(bool mute);

/* Set DAC output volume (0-100) */
esp_err_t es8311_voice_volume_set_level(int volume);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_CYD_S3 */
