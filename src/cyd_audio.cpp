/*
 * CYD Audio — I2S sine-wave tone generator for Freenove ESP32-S3 2.8" CYD
 *
 * Initializes the ES8311 codec via I2C (Wire, already started by display_touch_init)
 * and the I2S peripheral. A FreeRTOS background task continuously writes audio
 * samples: sine wave when a tone is active, silence otherwise.
 *
 * Uses the legacy ESP-IDF driver/i2s.h API (compatible with espressif32 platform
 * / Arduino-ESP32 2.x).
 *
 * Compiled only for BOARD_CYD_S3 builds.
 */

#ifdef BOARD_CYD_S3

#include "cyd_audio.h"
#include "es8311_driver.h"
#include "board_config.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// I2S pin definitions (CYD hardware)
#define I2S_MCK_PIN   4
#define I2S_BCK_PIN   5
#define I2S_WS_PIN    7
#define I2S_DOUT_PIN  8
#define I2S_DIN_PIN   6

// Audio parameters
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_BUF_SAMPLES   64   // samples per channel per buffer write

static TaskHandle_t _toneTaskHandle = NULL;
static volatile bool _initialized = false;

// Tone state (written by cyd_tone/cyd_noTone, read by task)
static volatile bool _toneActive = false;
static volatile unsigned int _toneFreq = 0;
static volatile unsigned long _toneEndMs = 0;   // 0 = indefinite
static volatile uint8_t _volume = 200;          // 0-255

// Pre-computed sine table (256 entries, amplitude +/-1.0)
static float _sinTable[256];
static bool _sinTableReady = false;

static void buildSinTable() {
    if (_sinTableReady) return;
    for (int i = 0; i < 256; i++) {
        _sinTable[i] = sinf(2.0f * M_PI * i / 256.0f);
    }
    _sinTableReady = true;
}

// Background task: generates audio and writes to I2S
static void toneTask(void* param) {
    int16_t buf[AUDIO_BUF_SAMPLES * 2]; // stereo: L, R, L, R, ...
    float phase = 0.0f;
    size_t bytes_written;

    while (true) {
        if (_toneActive && _toneFreq > 0) {
            // Check duration expiry
            if (_toneEndMs > 0 && millis() >= _toneEndMs) {
                _toneActive = false;
                continue;
            }

            // Generate sine wave
            float phaseInc = (float)_toneFreq / (float)AUDIO_SAMPLE_RATE;
            float amplitude = (float)_volume / 255.0f * 24000.0f; // ~73% of int16 max

            for (int i = 0; i < AUDIO_BUF_SAMPLES; i++) {
                int idx = (int)(phase * 256.0f) & 0xFF;
                int16_t sample = (int16_t)(_sinTable[idx] * amplitude);
                buf[i * 2]     = sample; // left
                buf[i * 2 + 1] = sample; // right
                phase += phaseInc;
                if (phase >= 1.0f) phase -= 1.0f;
            }

            i2s_write(I2S_NUM_0, buf, sizeof(buf), &bytes_written, portMAX_DELAY);
        } else {
            // Silence — write zeros
            memset(buf, 0, sizeof(buf));
            i2s_write(I2S_NUM_0, buf, sizeof(buf), &bytes_written, portMAX_DELAY);
            phase = 0.0f;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void cyd_audio_init() {
    if (_initialized) return;
    _initialized = true;

    buildSinTable();

    // Amp OFF during init (active-LOW: LOW=on, HIGH=off)
    pinMode(SPEAKER_EN_PIN, OUTPUT);
    digitalWrite(SPEAKER_EN_PIN, HIGH);  // amp off during setup

    // Configure I2S driver (legacy API)
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = AUDIO_SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 8;
    i2s_config.dma_buf_len = AUDIO_BUF_SAMPLES;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[CYD-AUDIO] I2S driver install FAILED: %d\n", err);
        _initialized = false;
        return;
    }

    // Set I2S pins
    i2s_pin_config_t pin_config = {};
    pin_config.mck_io_num = I2S_MCK_PIN;
    pin_config.bck_io_num = I2S_BCK_PIN;
    pin_config.ws_io_num = I2S_WS_PIN;
    pin_config.data_out_num = I2S_DOUT_PIN;
    pin_config.data_in_num = I2S_DIN_PIN;

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[CYD-AUDIO] I2S set pin FAILED: %d\n", err);
        _initialized = false;
        return;
    }

    // Clear DMA buffer
    i2s_zero_dma_buffer(I2S_NUM_0);

    // Initialize ES8311 codec (Wire already running from display_touch_init)
    err = es8311_codec_init();
    if (err != ESP_OK) {
        Serial.printf("[CYD-AUDIO] ES8311 init FAILED: %d\n", err);
        _initialized = false;
        return;
    }
    es8311_set_voice_mute(false);

    // Enable speaker amplifier (active-LOW: LOW=enabled)
    digitalWrite(SPEAKER_EN_PIN, LOW);

    // Start background tone task on core 1
    xTaskCreatePinnedToCore(toneTask, "cydTone", 4096, NULL, 2, &_toneTaskHandle, 1);
    Serial.println("[CYD-AUDIO] Audio ready");
}

void cyd_tone(uint8_t pin, unsigned int frequency, unsigned long duration) {
    (void)pin; // ignored — always uses I2S
    if (!_initialized) return;
    _toneFreq = frequency;
    _toneEndMs = (duration > 0) ? (millis() + duration) : 0;
    _toneActive = true;
}

void cyd_noTone(uint8_t pin) {
    (void)pin;
    _toneActive = false;
}

void cyd_set_volume(uint8_t vol_0_255) {
    _volume = vol_0_255;
}

#endif /* BOARD_CYD_S3 */
