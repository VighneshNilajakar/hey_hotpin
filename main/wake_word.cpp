/*
 * @file wake_word.c
 * @brief Edge Impulse "Hey Hotpin" wake word detector for AI-Thinker ESP32-CAM.
 *
 * Sliding 1 s window @ 16 kHz, one inference per 250 ms stride (4 Hz).
 * Label order comes from the model export and is verified at boot.
 */

#include "wake_word.h"
#include "audio_driver.h"
#include "board_pins.h"

#include <string.h>

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "WAKE_WORD";

/* ── Model geometry (from model-parameters/model_metadata.h) ────────────────── */

#define WW_WINDOW_SAMPLES     EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE   /* 16000 */
#define WW_STRIDE_SAMPLES     EI_CLASSIFIER_SLICE_SIZE              /*  4000 */
#define WW_LABEL_COUNT        EI_CLASSIFIER_LABEL_COUNT
#define WW_FREQUENCY          EI_CLASSIFIER_FREQUENCY              /* 16000 */

/* ── Tuning (see guide section 16) ──────────────────────────────────────────── */

#ifndef CONFIG_WAKE_WORD_THRESHOLD_PCT
#define CONFIG_WAKE_WORD_THRESHOLD_PCT   82
#endif
#define WW_THRESHOLD          (CONFIG_WAKE_WORD_THRESHOLD_PCT / 100.0f)
#define WW_VOTE_REQUIRED      2
#define WW_VOTE_WINDOW        5

#define WW_TASK_STACK         8192
#define WW_TASK_PRIORITY      5
#define WW_TASK_CORE          1

/* ── State ──────────────────────────────────────────────────────────────────── */

static int16_t s_window[WW_WINDOW_SAMPLES];      /* 32 KB, internal DRAM */
static int16_t s_stride_buf[WW_STRIDE_SAMPLES];  /*  8 KB, internal DRAM */
static size_t  s_window_pos;

static uint8_t s_vote_history[WW_VOTE_WINDOW];
static uint8_t s_vote_head;

static volatile bool s_enabled;
static volatile bool s_paused;
static TaskHandle_t  s_task;

static wake_word_trigger_cb_t s_on_trigger;

/* ── Status LED ─────────────────────────────────────────────────────────────── */

static volatile TickType_t s_led_off_at;

static void led_write(bool on)
{
#if STATUS_LED_GPIO >= 0
    const int level = STATUS_LED_ACTIVE_LOW ? (on ? 0 : 1) : (on ? 1 : 0);
    gpio_set_level((gpio_num_t)STATUS_LED_GPIO, level);
#endif
}

static void led_init(void)
{
#if STATUS_LED_GPIO >= 0
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
#endif
    led_write(false);
    s_led_off_at = 0;
}

/* Lit for a fixed window, then the task loop switches it back off. */
static void led_flash(void)
{
#if STATUS_LED_GPIO >= 0
    led_write(true);
    s_led_off_at = xTaskGetTickCount() + pdMS_TO_TICKS(STATUS_LED_ON_MS);
#endif
}

static void led_service(void)
{
#if STATUS_LED_GPIO >= 0
    if (s_led_off_at != 0 && (int32_t)(xTaskGetTickCount() - s_led_off_at) >= 0) {
        led_write(false);
        s_led_off_at = 0;
    }
#endif
}

/* ── Signal source handed to the Edge Impulse SDK ───────────────────────────── */

static int wake_word_get_signal_data(size_t offset, size_t length, float *out_ptr)
{
    if (offset + length > WW_WINDOW_SAMPLES) {
        return ei::EIDSP_SIGNAL_SIZE_MISMATCH;
    }
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = (float)s_window[offset + i] / 32768.0f;
    }
    return EIDSP_OK;
}

static void wake_word_fill_signal(signal_t *signal)
{
    memset(signal, 0, sizeof(signal_t));
    signal->total_length = WW_WINDOW_SAMPLES;
    signal->get_data     = wake_word_get_signal_data;
}

/* ── Vote filter ────────────────────────────────────────────────────────────── */

static bool vote_filter_update(bool triggered)
{
    s_vote_history[s_vote_head] = triggered ? 1 : 0;
    s_vote_head = (s_vote_head + 1) % WW_VOTE_WINDOW;

    uint8_t votes = 0;
    for (int i = 0; i < WW_VOTE_WINDOW; i++) {
        votes += s_vote_history[i];
    }
    return votes >= WW_VOTE_REQUIRED;
}

static void vote_filter_clear(void)
{
    memset(s_vote_history, 0, sizeof(s_vote_history));
    s_vote_head = 0;
}

/* ── Inference ──────────────────────────────────────────────────────────────── */

static int find_label(const char *name)
{
    for (int i = 0; i < WW_LABEL_COUNT; i++) {
        if (strcmp(ei_classifier_inferencing_categories[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Running peak over the last WW_PEAK_REPORT windows, so a phrase that scores
 * below threshold stays visible instead of looking like a dead mic. */
static float    s_peak_recent;
static uint32_t s_since_report;
static int32_t  s_audio_peak;
static uint32_t s_audio_rms;
#define WW_PEAK_REPORT 12

static void wake_word_log_result(const ei_impulse_result_t *result, int keyword_ix)
{
    const int64_t total_us = result->timing.dsp + result->timing.classification;
    const float confidence = result->classification[keyword_ix].value;

    if (confidence > s_peak_recent) {
        s_peak_recent = confidence;
    }

    ESP_LOGI(TAG, "%-10s %5.1f%%  noise %5.1f%%  unknown %5.1f%%  (dsp %d ms, nn %d ms)",
             ei_classifier_inferencing_categories[keyword_ix],
             confidence * 100.0f,
             result->classification[find_label("noise")].value * 100.0f,
             result->classification[find_label("unknown")].value * 100.0f,
             result->timing.dsp, result->timing.classification);
    if (total_us > 100000) {
        ESP_LOGW(TAG, "inference took %lld ms - over the 100 ms budget",
                 (long long)(total_us / 1000));
    }

    if (++s_since_report >= WW_PEAK_REPORT) {
        const int32_t p = audio_driver_last_peak();
        const uint32_t r = audio_driver_last_rms();
        if (p > s_audio_peak) {
            s_audio_peak = p;
        }
        if (r > s_audio_rms) {
            s_audio_rms = r;
        }
        s_since_report = 0;
        ESP_LOGI(TAG, "PEAK %d windows: hey_hotpin %.1f%% (thr %.0f%%) | audio peak %d rms %d of 32768",
                 WW_PEAK_REPORT, s_peak_recent * 100.0f,
                 (double)(WW_THRESHOLD * 100.0f), s_audio_peak, s_audio_rms);
        s_peak_recent = 0.0f;
        s_audio_peak  = 0;
        s_audio_rms   = 0;
    }
}

static bool wake_word_run_once(void)
{
    const int keyword_ix = 0;   /* verified at boot: "hey_hotpin" */

    signal_t signal;
    ei_impulse_result_t result;
    wake_word_fill_signal(&signal);

    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    if (err != EI_IMPULSE_OK) {
        ESP_LOGE(TAG, "run_classifier failed: %d", (int)err);
        return false;
    }

    wake_word_log_result(&result, keyword_ix);

    float confidence = result.classification[keyword_ix].value;
    bool fired = vote_filter_update(confidence >= WW_THRESHOLD);

    if (fired) {
        ESP_LOGW(TAG, "WAKE WORD DETECTED (hey_hotpin=%.3f, threshold=%.2f)",
                 confidence, (double)WW_THRESHOLD);
        vote_filter_clear();
        led_flash();
        if (s_on_trigger) {
            s_on_trigger(confidence);
        }
    }
    return fired;
}

/* ── Task ───────────────────────────────────────────────────────────────────── */

static void wake_word_task(void *pv)
{
    (void)pv;

    ESP_LOGI(TAG, "task running on core %d, stack free %u bytes",
             (int)xPortGetCoreID(), (unsigned)uxTaskGetStackHighWaterMark(NULL));

    int16_t *stride_buf = s_stride_buf;

    memset(s_window, 0, sizeof(s_window));
    s_window_pos = 0;

    bool logged_first_stride = false;

    while (true) {
        if (!s_enabled || s_paused) {
            led_write(false);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        led_service();

        esp_err_t ret = audio_driver_read(stride_buf, WW_STRIDE_SAMPLES, 500);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s read failed (%s)", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!logged_first_stride) {
            ESP_LOGI(TAG, "first stride: %d %d %d %d ... %d",
                     stride_buf[0], stride_buf[1], stride_buf[2], stride_buf[3],
                     stride_buf[WW_STRIDE_SAMPLES - 1]);
            logged_first_stride = true;
        }

        for (size_t i = 0; i < WW_STRIDE_SAMPLES; i++) {
            s_window[(s_window_pos + i) % WW_WINDOW_SAMPLES] = stride_buf[i];
        }
        s_window_pos = (s_window_pos + WW_STRIDE_SAMPLES) % WW_WINDOW_SAMPLES;

        wake_word_run_once();

        vTaskDelay(1);   /* let the idle task / WDT run */
    }
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

esp_err_t wake_word_init(void)
{
    memset(s_window, 0, sizeof(s_window));
    memset(s_vote_history, 0, sizeof(s_vote_history));
    s_window_pos = 0;
    s_vote_head  = 0;
    s_enabled    = false;
    s_paused     = false;
    s_task       = NULL;
    s_peak_recent   = 0.0f;
    s_since_report  = 0;
    s_audio_peak    = 0;
    s_audio_rms     = 0;

    led_init();
    ESP_LOGI(TAG, "status LED on GPIO%d (%s, %d ms)",
             STATUS_LED_GPIO, STATUS_LED_ACTIVE_LOW ? "active low" : "active high",
             STATUS_LED_ON_MS);

    ESP_LOGI(TAG, "model: project %d, impulse %d",
             EI_CLASSIFIER_PROJECT_ID, 1);
    ESP_LOGI(TAG, "window %d samples (%d ms @ %d Hz), stride %d samples (%d ms)",
             WW_WINDOW_SAMPLES, (int)(WW_WINDOW_SAMPLES * 1000 / WW_FREQUENCY),
             WW_FREQUENCY, WW_STRIDE_SAMPLES,
             (int)(WW_STRIDE_SAMPLES * 1000 / WW_FREQUENCY));
    ESP_LOGI(TAG, "threshold %.2f, vote %d/%d",
             (double)WW_THRESHOLD, WW_VOTE_REQUIRED, WW_VOTE_WINDOW);

    for (int i = 0; i < WW_LABEL_COUNT; i++) {
        ESP_LOGI(TAG, "Label[%d] = %s", i, ei_classifier_inferencing_categories[i]);
    }
    if (find_label("hey_hotpin") != 0) {
        ESP_LOGE(TAG, "expected 'hey_hotpin' at index 0 - check the label order above");
    }

    return ESP_OK;
}

esp_err_t wake_word_start(wake_word_trigger_cb_t on_trigger)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_on_trigger = on_trigger;
    s_enabled    = true;

    BaseType_t ret = xTaskCreatePinnedToCore(wake_word_task, "wake_word",
                                             WW_TASK_STACK, NULL,
                                             WW_TASK_PRIORITY, &s_task,
                                             WW_TASK_CORE);
    if (ret != pdPASS) {
        s_enabled = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

void wake_word_pause(void)
{
    s_paused = true;
    vote_filter_clear();
}

void wake_word_resume(void)
{
    memset(s_window, 0, sizeof(s_window));
    s_window_pos = 0;
    vote_filter_clear();
    s_paused = false;
}

void wake_word_stop(void)
{
    s_enabled = false;
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
}

/* ── Self test ──────────────────────────────────────────────────────────────── */

void wake_word_selftest(uint32_t iterations)
{
    const int keyword_ix = 0;

    /* A quiet room is what "noise" should look like. Sweep the window so the
     * MFCC front end sees real frame boundaries rather than one DC block. */
    for (size_t i = 0; i < WW_WINDOW_SAMPLES; i++) {
        s_window[i] = (int16_t)((((int32_t)i * 37) % 97) - 48);
    }

    uint32_t dsp_max = 0, nn_max = 0;
    int64_t   wall_total = 0;

    for (uint32_t it = 0; it < iterations; it++) {
        signal_t signal;
        ei_impulse_result_t result;
        wake_word_fill_signal(&signal);

        int64_t t0 = esp_timer_get_time();
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
        int64_t wall = esp_timer_get_time() - t0;

        if (err != EI_IMPULSE_OK) {
            ESP_LOGE(TAG, "selftest iteration %u failed: %d", it, (int)err);
            return;
        }

        wall_total += wall;
        if ((uint32_t)result.timing.dsp  > dsp_max) dsp_max = result.timing.dsp;
        if ((uint32_t)result.timing.classification > nn_max) nn_max = result.timing.classification;

        ESP_LOGI(TAG, "selftest %2u/%u: %s=%.3f noise=%.3f unknown=%.3f  dsp=%d ms nn=%d ms wall=%lld ms",
                 it + 1, iterations,
                 ei_classifier_inferencing_categories[keyword_ix],
                 result.classification[keyword_ix].value,
                 result.classification[find_label("noise")].value,
                 result.classification[find_label("unknown")].value,
                 result.timing.dsp, result.timing.classification,
                 (long long)(wall / 1000));
    }

    ESP_LOGI(TAG, "selftest done: mean wall %lld ms, worst dsp %u ms, worst nn %u ms",
             (long long)(wall_total / (int64_t)iterations / 1000), dsp_max, nn_max);

    if (dsp_max + nn_max > 100) {
        ESP_LOGE(TAG, "DSP+NN exceeds the 100 ms budget");
    } else {
        ESP_LOGI(TAG, "DSP+NN within the 100 ms budget");
    }
}
