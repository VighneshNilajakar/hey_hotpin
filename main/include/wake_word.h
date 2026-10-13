#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wake_word_trigger_cb_t)(float confidence);

esp_err_t wake_word_init(void);
esp_err_t wake_word_start(wake_word_trigger_cb_t on_trigger);
void      wake_word_pause(void);
void      wake_word_resume(void);
void      wake_word_stop(void);

/* Runs N inferences on a synthetic window and logs DSP/NN timing. Used to
 * validate the deployment without a microphone attached. */
void      wake_word_selftest(uint32_t iterations);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_WORD_H */
