#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * INMP441 I2S microphone capture.
 *
 * The driver converts the microphone's 24-bit left-justified payload into
 * int16 PCM in [-32768, 32767] and hands out `sample_count` samples per call.
 * Callers do not need to know anything about the 32-bit I2S slot format.
 */
esp_err_t audio_driver_init(void);

/*
 * Reads exactly `sample_count` int16 samples. Blocks until the samples are
 * available or `timeout_ms` elapses.
 */
esp_err_t audio_driver_read(int16_t *dest, size_t sample_count, size_t timeout_ms);

/* Level telemetry from the most recent read, as int16 units (32768 = full scale).
 * Use these to pick MIC_GAIN_SHIFT instead of guessing. */
int32_t  audio_driver_last_peak(void);
uint32_t audio_driver_last_rms(void);

void audio_driver_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_DRIVER_H */
