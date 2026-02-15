/*
 * ES8311 Audio Codec Driver — adapted from Freenove ESP32-S3 Display example
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compiled only for BOARD_CYD_S3. Uses Wire for I2C communication with the
 * ES8311 codec at address 0x18. Wire must be initialized before calling
 * es8311_codec_init() (display_init() -> display_touch_init() does this).
 */

#ifdef BOARD_CYD_S3

#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "es8311_driver.h"
#include "es8311_reg.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

static const char *TAG = "ES8311";

// ---------------------------------------------------------------------------
// I2C register read/write via Wire
// ---------------------------------------------------------------------------
static inline esp_err_t es8311_write_reg(uint8_t reg_addr, uint8_t data)
{
    Wire.beginTransmission(ES8311_ADDRESS);
    Wire.write(reg_addr);
    Wire.write(data);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "I2C Write Error: %d", err);
    return ESP_FAIL;
}

static inline esp_err_t es8311_read_reg(uint8_t reg_addr, uint8_t *reg_value)
{
    Wire.beginTransmission(ES8311_ADDRESS);
    Wire.write(reg_addr);
    uint8_t err = Wire.endTransmission(false);
    if (err != 0) {
        ESP_LOGE(TAG, "I2C Read (Write addr) Error: %d", err);
        return ESP_FAIL;
    }
    uint8_t len = Wire.requestFrom((uint16_t)ES8311_ADDRESS, (uint8_t)1);
    if (len == 1) {
        *reg_value = Wire.read();
        return ESP_OK;
    }
    ESP_LOGE(TAG, "I2C Read Error: No data");
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Clock coefficient table
// ---------------------------------------------------------------------------
struct _coeff_div {
    uint32_t mclk;
    uint32_t rate;
    uint8_t pre_div;
    uint8_t pre_multi;
    uint8_t adc_div;
    uint8_t dac_div;
    uint8_t fs_mode;
    uint8_t lrck_h;
    uint8_t lrck_l;
    uint8_t bclk_div;
    uint8_t adc_osr;
    uint8_t dac_osr;
};

static const struct _coeff_div coeff_div[] = {
    /* mclk       rate   pre_div mult adc_div dac_div fs lrch  lrcl  bckdiv osr */
    /* 8k */
    {12288000, 8000, 0x06, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 8000, 0x03, 0x01, 0x03, 0x03, 0x00, 0x05, 0xff, 0x18, 0x10, 0x10},
    {16384000, 8000, 0x08, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000, 8000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 8000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000, 8000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 8000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2048000, 8000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 8000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1024000, 8000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    /* 16k */
    {12288000, 16000, 0x03, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 16000, 0x03, 0x01, 0x03, 0x03, 0x00, 0x02, 0xff, 0x0c, 0x10, 0x10},
    {16384000, 16000, 0x04, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000, 16000, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 16000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000, 16000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 16000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2048000, 16000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 16000, 0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1024000, 16000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    /* 22.05k */
    {11289600, 22050, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800, 22050, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2822400, 22050, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1411200, 22050, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    /* 44.1k */
    {11289600, 44100, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {5644800, 44100, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2822400, 44100, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1411200, 44100, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    /* 48k */
    {12288000, 48000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 48000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 48000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 48000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {1536000, 48000, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
};

static int get_coeff(uint32_t mclk, uint32_t rate)
{
    for (int i = 0; i < (int)(sizeof(coeff_div) / sizeof(coeff_div[0])); i++) {
        if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Clock divider configuration
// ---------------------------------------------------------------------------
static esp_err_t es8311_sample_frequency_config(int mclk_frequency, int sample_frequency)
{
    uint8_t regv;
    int coeff = get_coeff(mclk_frequency, sample_frequency);
    if (coeff < 0) {
        ESP_LOGE(TAG, "Unable to configure sample rate %dHz with %dHz MCLK", sample_frequency, mclk_frequency);
        return ESP_ERR_INVALID_ARG;
    }
    const struct _coeff_div *const sel = &coeff_div[coeff];

    ESP_RETURN_ON_ERROR(es8311_read_reg(ES8311_CLK_MANAGER_REG02, &regv), TAG, "");
    regv &= 0x07;
    regv |= (sel->pre_div - 1) << 5;
    regv |= sel->pre_multi << 3;
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG02, regv), TAG, "");

    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG03, (sel->fs_mode << 6) | sel->adc_osr), TAG, "");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG04, sel->dac_osr), TAG, "");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG05, ((sel->adc_div - 1) << 4) | (sel->dac_div - 1)), TAG, "");

    ESP_RETURN_ON_ERROR(es8311_read_reg(ES8311_CLK_MANAGER_REG06, &regv), TAG, "");
    regv &= 0xE0;
    if (sel->bclk_div < 19) {
        regv |= (sel->bclk_div - 1);
    } else {
        regv |= sel->bclk_div;
    }
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG06, regv), TAG, "");

    ESP_RETURN_ON_ERROR(es8311_read_reg(ES8311_CLK_MANAGER_REG07, &regv), TAG, "");
    regv &= 0xC0;
    regv |= sel->lrck_h;
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG07, regv), TAG, "");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG08, sel->lrck_l), TAG, "");

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t es8311_codec_init(void)
{
    uint8_t reg00;

    /* Reset */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, 0x1F), TAG, "reset write");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, 0x00), TAG, "reset clear");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, 0x80), TAG, "power-on");

    /* Clock: MCLK from MCLK pin, no inversion */
    uint8_t reg01 = 0x3F; /* enable all clocks */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG01, reg01), TAG, "");

    /* Configure clock dividers for 16kHz sample rate */
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(ES8311_MCLK_FREQ_HZ, ES8311_SAMPLE_RATE), TAG, "");

    /* Slave mode, I2S format, 16-bit resolution */
    ESP_RETURN_ON_ERROR(es8311_read_reg(ES8311_RESET_REG00, &reg00), TAG, "");
    reg00 &= 0xBF;
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, reg00), TAG, "");

    /* SDP: 16-bit resolution in and out */
    uint8_t reg09 = (3 << 2); /* 16-bit */
    uint8_t reg0a = (3 << 2); /* 16-bit */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SDPIN_REG09, reg09), TAG, "");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SDPOUT_REG0A, reg0a), TAG, "");

    /* Power up analog circuitry */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01), TAG, "");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02), TAG, "");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG12, 0x00), TAG, "");  /* power-up DAC */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG13, 0x10), TAG, "");  /* enable HP drive */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG1C, 0x6A), TAG, "");     /* ADC EQ bypass */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_DAC_REG37, 0x08), TAG, "");     /* DAC EQ bypass */

    /* Set default volume */
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set_level(ES8311_VOICE_VOLUME), TAG, "");

    /* Configure microphone (analog, max PGA gain) */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG17, 0xC8), TAG, "");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG14, 0x1A), TAG, "");

    ESP_LOGI(TAG, "ES8311 codec initialized (16kHz, 16-bit, slave)");
    return ESP_OK;
}

void es8311_set_voice_mute(bool mute)
{
    uint8_t reg31;
    if (es8311_read_reg(ES8311_DAC_REG31, &reg31) != ESP_OK) return;
    if (mute) {
        reg31 |= BIT(6) | BIT(5);
    } else {
        reg31 &= ~(BIT(6) | BIT(5));
    }
    es8311_write_reg(ES8311_DAC_REG31, reg31);
}

esp_err_t es8311_voice_volume_set_level(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    int reg32 = (volume == 0) ? 0 : ((volume * 256 / 100) - 1);
    return es8311_write_reg(ES8311_DAC_REG32, (uint8_t)reg32);
}

#endif /* BOARD_CYD_S3 */
