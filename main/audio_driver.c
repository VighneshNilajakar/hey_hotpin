#include "audio_driver.h"
#include "board_pins.h"

#include <math.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_DRV";

#define MIC_SAMPLE_RATE        16000
#define MIC_DMA_DESC_NUM       8
#define MIC_DMA_FRAME_NUM      320      /* frames -> 20 ms per DMA descriptor */
#define MIC_MAX_READ_SAMPLES   8000

static i2s_chan_handle_t s_rx_chan;
static int32_t          *s_raw_buf;
static bool              s_initialised;

/*
 * On the ESP32 the window at 0x40080000..0x400FFFFF is iram0 (execute+read,
 * no write). heap_init reports its tail as an "IRAM" heap and
 * MALLOC_CAP_INTERNAL can hand it back, but touching it raises
 * LoadStoreProhibited. Anything the DSP or DMA reads must live in internal
 * DRAM, i.e. below 0x40080000.
 */
static inline bool mic_buf_is_writable(uintptr_t addr)
{
    return addr >= 0x3FF00000ul && addr < 0x40080000ul;
}

/* INMP441 drives 24 significant bits left-justified inside a 32-bit I2S slot.
 * A one-pole high-pass strips the microphone's DC offset before the samples
 * reach the MFCC front end. */
static int64_t s_hp_state;
static int64_t s_hp_prev;

static int32_t s_last_peak;
static uint32_t s_last_rms;

static inline int16_t mic_slot_to_pcm16(int32_t slot)
{
    const int64_t raw = slot;

    s_hp_state = (raw - s_hp_prev) + s_hp_state - (s_hp_state >> 6);
    s_hp_prev = raw;

    int32_t v = (int32_t)(s_hp_state >> MIC_GAIN_SHIFT);
    if (v > 32767) {
        v = 32767;
    } else if (v < -32768) {
        v = -32768;
    }
    return (int16_t)v;
}

esp_err_t audio_driver_init(void)
{
    if (s_initialised) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = MIC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = MIC_DMA_FRAME_NUM;
    chan_cfg.auto_clear    = false;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_GPIO,
            .ws   = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = (MIC_CHANNEL_LEFT) ? I2S_STD_SLOT_LEFT
                                                   : I2S_STD_SLOT_RIGHT;

    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return err;
    }

    s_raw_buf = heap_caps_malloc(MIC_MAX_READ_SAMPLES * sizeof(int32_t),
                                 MALLOC_CAP_DMA);
    if (s_raw_buf == NULL || !mic_buf_is_writable((uintptr_t)s_raw_buf)) {
        ESP_LOGE(TAG, "no usable internal DMA memory for the I2S staging buffer (%p)",
                 (void *)s_raw_buf);
        free(s_raw_buf);
        s_raw_buf = NULL;
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialised = true;

    ESP_LOGI(TAG, "INMP441 ready: BCLK=%d WS=%d DIN=%d, %d Hz, %s channel, 32-bit slots",
             MIC_BCLK_GPIO, MIC_WS_GPIO, MIC_DIN_GPIO, MIC_SAMPLE_RATE,
             MIC_CHANNEL_LEFT ? "LEFT" : "RIGHT");
    ESP_LOGI(TAG, "dma desc=%d frame=%d (%d ms), staging buffer %p (%u bytes)",
             MIC_DMA_DESC_NUM, MIC_DMA_FRAME_NUM,
             (int)(MIC_DMA_DESC_NUM * MIC_DMA_FRAME_NUM * 1000 / MIC_SAMPLE_RATE),
             s_raw_buf, (unsigned)(MIC_MAX_READ_SAMPLES * sizeof(int32_t)));
    return ESP_OK;
}

esp_err_t audio_driver_read(int16_t *dest, size_t sample_count, size_t timeout_ms)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count > MIC_MAX_READ_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes_want = sample_count * sizeof(int32_t);
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, s_raw_buf, bytes_want,
                                     &bytes_read, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        return err;
    }
    if (bytes_read != bytes_want) {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < sample_count; i++) {
        dest[i] = mic_slot_to_pcm16(s_raw_buf[i]);
    }

    /* Cheap level telemetry so the gain can be tuned from real numbers instead
     * of guesswork. peak is the absolute maximum, rms the per-stride average. */
    int32_t peak = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int32_t a = dest[i];
        if (a < 0) {
            a = -a;
        }
        if (a > peak) {
            peak = a;
        }
        sum += (uint64_t)(dest[i] * dest[i]);
    }
    s_last_peak = peak;
    s_last_rms  = (uint32_t)sqrtf((float)(sum / sample_count));

    return ESP_OK;
}

int32_t audio_driver_last_peak(void)
{
    return s_last_peak;
}

uint32_t audio_driver_last_rms(void)
{
    return s_last_rms;
}

void audio_driver_deinit(void)
{
    if (!s_initialised) {
        return;
    }
    i2s_channel_disable(s_rx_chan);
    i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    free(s_raw_buf);
    s_raw_buf = NULL;
    s_initialised = false;
}
