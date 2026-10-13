#include "audio_driver.h"
#include "board_pins.h"
#include "wake_word.h"

#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "MAIN";

static void log_memory_budget(void)
{
    size_t internal_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t dma_free         = heap_caps_get_free_size(MALLOC_CAP_DMA);

    ESP_LOGI(TAG, "MEM  internal free    : %u bytes", (unsigned)internal_free);
    ESP_LOGI(TAG, "MEM  internal largest : %u bytes (pass: > 80 KB)", (unsigned)internal_largest);
    ESP_LOGI(TAG, "MEM  DMA free         : %u bytes (pass: > 32 KB)", (unsigned)dma_free);

    if (internal_largest < 80 * 1024) {
        ESP_LOGE(TAG, "MEM  largest internal block below the 80 KB pass mark");
    }
    if (dma_free < 32 * 1024) {
        ESP_LOGE(TAG, "MEM  DMA free below the 32 KB pass mark");
    }
}

static void on_wake_word(float confidence)
{
    ESP_LOGW(TAG, "callback: wake word fired at confidence %.3f", (double)confidence);
    /* Hook here: post SYSTEM_EVENT_WAKE_WORD, pause STT, start the camera, ... */
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-Waker \"Hey Hotpin\" wake word detector");
    ESP_LOGW(TAG, "mic is on GPIO%d/%d/%d - if those are flash pins, expect trouble",
             MIC_BCLK_GPIO, MIC_WS_GPIO, MIC_DIN_GPIO);

    /* NVS is deliberately not initialised: it writes to flash, and the mic may
     * be sitting on the flash bus. Skipping it keeps the flash idle. */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %d rev %d, %d core(s), flash mode DIO, configured %d MHz",
             chip.model, chip.revision, chip.cores,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

#if CONFIG_WAKE_WORD_SELFTEST
    ESP_LOGI(TAG, "running self test (no microphone required)");
    ESP_ERROR_CHECK(wake_word_init());
    wake_word_selftest(5);
#endif

    ESP_ERROR_CHECK(audio_driver_init());

    ESP_ERROR_CHECK(wake_word_init());
    ESP_ERROR_CHECK(wake_word_start(on_wake_word));

    log_memory_budget();

    ESP_LOGI(TAG, "running - say \"Hey Hotpin\"");
}
