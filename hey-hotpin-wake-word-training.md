# Hey Hotpin — Wake Word Training Guide
**Edge Impulse · Classic ESP32 (Xtensa LX6, 160 MHz, PSRAM enabled)**

---

## Contents

1. [Before you start](#1-before-you-start)
2. [Project setup](#2-project-setup)
3. [Data strategy](#3-data-strategy)
4. [Recording real samples](#4-recording-real-samples)
5. [Generating synthetic samples](#5-generating-synthetic-samples)
6. [Noise and unknown samples](#6-noise-and-unknown-samples)
7. [Dataset quality checks](#7-dataset-quality-checks)
8. [Impulse design](#8-impulse-design)
9. [MFCC block configuration](#9-mfcc-block-configuration)
10. [EON Tuner — find the optimal config](#10-eon-tuner--find-the-optimal-config)
11. [Neural network training](#11-neural-network-training)
12. [Model testing](#12-model-testing)
13. [Deployment — C++ library for ESP-IDF](#13-deployment--c-library-for-esp-idf)
14. [ESP-IDF integration](#14-esp-idf-integration)
15. [Continuous inference with sliding window](#15-continuous-inference-with-sliding-window)
16. [Vote filter — reducing false accepts](#16-vote-filter--reducing-false-accepts)
17. [Memory and CPU verification](#17-memory-and-cpu-verification)
18. [Iteration checklist](#18-iteration-checklist)

---

## 1. Before you start

**Accounts needed**

- Edge Impulse account (free Developer plan is sufficient): https://edgeimpulse.com/signup
- Google Cloud account with Text-to-Speech API enabled (first 1 M chars/month free on WaveNet voices): https://cloud.google.com/text-to-speech

**Hardware**

- AI-Thinker ESP32-CAM with PSRAM confirmed present (`esp_psram_get_size()` returns > 0)
- INMP441 microphone on I2S (BCLK GPIO14, WS GPIO15, DIN GPIO2)
- CPU fixed at 160 MHz in `sdkconfig.defaults`
- Brownout detector re-enabled (`CONFIG_ESP_BROWNOUT_DET=y`)

**Software**

```bash
pip install edge-impulse-cli      # EI CLI for uploading and running impulse
pip install pydub
pip install google-cloud-texttospeech
pip install requests
```

Edge Impulse CLI docs: https://docs.edgeimpulse.com/tools/clis/edge-impulse-cli

---

## 2. Project setup

1. Log in to Edge Impulse Studio: https://studio.edgeimpulse.com
2. Click **Create new project** → name it `hotpin-wake-word`.
3. Go to **Dashboard → Project info → Target device**. Select **Custom board**. Set:
   - CPU: Xtensa LX6 (or pick Cortex-M4F @ 80 MHz as the closest proxy — latency estimates will be conservative)
   - RAM: 320 KB
   - Flash: 4 MB
4. Set **Sensor** to **Microphone**.

---

## 3. Data strategy

You need exactly **three** classes. Keep them balanced (roughly equal minutes per class).

| Class | Contents | Target minimum |
|---|---|---|
| `hey_hotpin` | Your wake phrase — all variations | 15 minutes |
| `unknown` | Other speech — words, sentences, similar sounds | 15 minutes |
| `noise` | Non-speech background — fans, music, TV, silence | 15 minutes |

**Why "Hey Hotpin" and not "Hotpin" alone**

"Hotpin" contains common phonemes ("hot", "pin", "hopping") that appear in everyday speech. A two-word phrase with a distinct onset ("Hey") gives the model more time-series structure to latch on to and raises the bar for false accepts significantly.

**Why three classes and not two**

A two-class model (keyword vs everything) tends to over-trigger. Separating `unknown` speech from `noise` gives the network a clearer decision boundary and lets you tune false accepts independently.

**Train/test split**: allocate 80% to training, 20% to test. Use **Dashboard → Perform train/test split** after uploading.

---

## 4. Recording real samples

Real samples recorded with your own INMP441 are the most valuable data. They match the exact mic, gain, and room acoustics of deployment.

**Steps**

1. In Edge Impulse Studio go to **Data acquisition**.
2. Set label to `hey_hotpin`, sensor to Microphone, frequency to **16000 Hz**, sample length to **10 seconds**.
3. Click **Start sampling**. Say "Hey Hotpin" roughly every 1.5 seconds with a short pause between each utterance. Fill 10 seconds.
4. After recording, click the three-dot menu on the sample and choose **Split sample**. Enable **Shift samples** (randomly pads the 1 s window around the utterance). Click **Split**.

**Recording tips for maximum accuracy**

- Record in the environment where the device will actually be used, not a quiet studio.
- Vary your distance: 0.3 m, 0.5 m, 1 m.
- Vary your volume: conversational, slightly louder, slightly quieter.
- Vary your pace: normal speed, slightly slower, slightly faster.
- Get at least 3–5 other speakers (colleagues, family). A model trained on one voice will fail on others.
- Use your phone's browser (https://docs.edgeimpulse.com/hardware/devices/mobile-phone) for faster data collection — it saves you wiring the board for data capture.
- Target **at least 200 real split samples** (about 3–4 minutes) before supplementing with synthetic data.

**Capture `unknown` samples the same way**

Record common words that sound like your phrase. Specifically include: "hey", "hot", "pin", "hotpot", "hot pink", "hey Siri", "OK Google", names starting with H, random sentences. Aim for variety. EI's pre-built unknown dataset (see section 6) supplements this well.

---

## 5. Generating synthetic samples

Synthetic data covers voice diversity (age, accent, gender, pitch) that is impractical to collect manually.

**Colab notebook (easiest)**

Open the official EI notebook directly:

[![Open in Colab](https://colab.research.google.com/assets/colab-badge.svg)](https://colab.research.google.com/github/edgeimpulse/notebooks/blob/main/notebooks/generate-keyword-spotting-dataset.ipynb)

**Local script**

Save as `generate_hey_hotpin.py` and run from your project directory.

```python
import os, json, time, io, random, requests
from pydub import AudioSegment
from google.cloud import texttospeech

# ── Configuration ────────────────────────────────────────────────────────────
os.environ['GOOGLE_APPLICATION_CREDENTIALS'] = 'path/to/your-gcp-credentials.json'

keyword = [{'string': 'Hey Hotpin', 'label': 'hey_hotpin'}]

# Use en-IN to reflect Indian English accent (primary user)
# Keep en-US / en-GB for cross-accent robustness
languages    = ['en-IN', 'en-US', 'en-GB', 'en-AU']
pitches      = [-4, -2, 0, 2, 4]       # semitones
genders      = ['FEMALE', 'MALE', 'NEUTRAL']
speakingRates = [0.85, 0.95, 1.0, 1.05, 1.15]

out_length      = 1          # seconds per sample
count           = 500        # total synthetic samples
voice_dir       = 'out-wav'
output_folder   = 'out-noisy'
num_copies      = 3          # noisy copies per clean sample
max_noise_level = -8         # dBFS

# ── Background noise (Creative Commons) ──────────────────────────────────────
noise_url = 'https://cdn.freesound.org/previews/483/483561_10201334-lq.ogg'

# ── Setup ─────────────────────────────────────────────────────────────────────
os.makedirs(voice_dir, exist_ok=True)
os.makedirs(output_folder, exist_ok=True)

response = requests.get(noise_url)
response.raise_for_status()
noise_audio = AudioSegment.from_file(io.BytesIO(response.content), format='ogg')

# ── Generate combinations ─────────────────────────────────────────────────────
all_opts = []
for p in pitches:
    for g in genders:
        for l in languages:
            for s in speakingRates:
                for kw in keyword:
                    all_opts.append({'pitch': p, 'gender': g, 'language': l,
                                     'speakingRate': s, 'text': kw['string'],
                                     'label': kw['label']})

if len(all_opts) * num_copies > count:
    step = (len(all_opts) * num_copies) // count
    all_opts = all_opts[::step]

print(f'Generating {len(all_opts) * num_copies} samples')

client = texttospeech.TextToSpeechClient()
downloaded_files = []

for ix, o in enumerate(all_opts):
    synthesis_input = texttospeech.SynthesisInput(text=o['text'])
    voice = texttospeech.VoiceSelectionParams(
        language_code=o['language'], ssml_gender=o['gender'])
    audio_config = texttospeech.AudioConfig(
        audio_encoding=texttospeech.AudioEncoding.LINEAR16,
        pitch=o['pitch'], speaking_rate=o['speakingRate'],
        sample_rate_hertz=16000)

    wav_name = (f"{voice_dir}/{o['label']}."
                f"{o['language']}-{o['gender']}-{o['pitch']}-{o['speakingRate']}.wav")

    if not os.path.exists(wav_name):
        resp = client.synthesize_speech(
            input=synthesis_input, voice=voice, audio_config=audio_config)
        with open(wav_name, 'wb') as f:
            f.write(resp.audio_content)
        time.sleep(0.3)

    voice_audio = AudioSegment.from_file(wav_name)
    diff = (out_length * 1000) - len(voice_audio)
    if diff > 0:
        pad_before = random.randint(0, diff)
        voice_audio = (AudioSegment.silent(pad_before)
                       + voice_audio
                       + AudioSegment.silent(diff - pad_before))

    for i in range(num_copies):
        out_name = (f"{o['label']}.{o['language']}-{o['gender']}-"
                    f"{o['pitch']}-{o['speakingRate']}_n{i+1}.wav")
        out_path = os.path.join(output_folder, out_name)
        if not os.path.exists(out_path):
            start = random.randint(0, len(noise_audio) - len(voice_audio))
            noise_seg = noise_audio[start: start + len(voice_audio)]
            noise_seg = noise_seg - abs(random.uniform(max_noise_level, 0))
            (voice_audio.overlay(noise_seg)).export(out_path, format='wav')
        downloaded_files.append({'path': out_name, 'label': o['label'],
                                  'category': 'split'})

with open(os.path.join(output_folder, 'input.json'), 'w') as f:
    json.dump({'version': 1, 'files': downloaded_files}, f)

print('Done. Upload with: edge-impulse-uploader --info-file out-noisy/input.json out-noisy/*')
```

**Upload to Edge Impulse**

```bash
cd out-noisy
edge-impulse-uploader --info-file input.json *.wav
```

The CLI auto-labels samples using `input.json`. Re-run if you need more voices or pitches.

---

## 6. Noise and unknown samples

Edge Impulse provides pre-built datasets you can import directly.

1. Go to **Data acquisition → Upload data** (cloud icon).
2. Download the keyword spotting dataset: https://docs.edgeimpulse.com/datasets/audio/keyword-spotting
3. Upload `noise` WAVs with label `noise` and `unknown` WAVs with label `unknown`.
4. 15–25 minutes per class is sufficient. Match your `hey_hotpin` count.

**Additional noise sources to record yourself**

- Your actual room at different times of day
- Your TV or music playing
- Fan or air conditioner
- Outdoor sound from a window

Record these as 10 s clips, upload with label `noise`, and split.

---

## 7. Dataset quality checks

Before training, verify the dataset is sound.

**Balance check**

Go to **Dashboard**. The class distribution bar chart should show roughly equal time in each class (within ±20%). If `noise` or `unknown` dwarfs `hey_hotpin`, the model will under-trigger.

**Train/test split**

Click **Perform train/test split** on the Dashboard. This randomly allocates 80% to training and 20% to test. Do this once, after all samples are uploaded.

**Feature explorer check**

After generating features (section 9), open the **Feature explorer** (3D scatter plot). `hey_hotpin` should form a visible cluster separate from the `unknown` and `noise` clusters. If all three classes overlap heavily, your training data quality is low — record more diverse samples or add more noise augmentation.

**Sample-level audit**

Click through 10–20 samples per class in Data acquisition. Listen to each. Delete:
- Samples where you said the phrase incorrectly
- Samples with clipping (waveform hitting ±1.0)
- Samples where the phrase was cut off (incomplete "Hey" or incomplete "Hotpin")
- Misclassified samples (noise labelled as keyword, etc.)

---

## 8. Impulse design

Go to **Create impulse**.

| Block | Setting |
|---|---|
| **Time series input** | Window size: 1000 ms · Window increase: 250 ms · Frequency: 16000 Hz · Zero-pad: on |
| **Processing block** | Audio (MFCC) |
| **Learning block** | Classification (Keras) |

Click **Save Impulse**.

**Why 1000 ms window**

"Hey Hotpin" spoken at a normal pace takes about 0.7–0.9 seconds. A 1 s window captures it fully regardless of timing. Shorter windows would clip the phrase. Longer windows add latency and compute cost.

**Why 250 ms stride**

At 250 ms stride the model runs 4 times per second. This gives a 4-inference window for your vote filter (section 16). The trade-off: 250 ms latency after the phrase ends before the last inference fires. You can reduce to 200 ms later if latency feels high.

---

## 9. MFCC block configuration

Click **MFCC** in the left menu.

**Recommended starting settings**

| Parameter | Value | Reason |
|---|---|---|
| Number of coefficients | 13 | Standard for speech. Enough frequency resolution for "Hey Hotpin". More adds RAM. |
| Frame length | 0.02 s (20 ms) | Standard analysis window for speech. |
| Frame stride | 0.01 s (10 ms) | 50% overlap. Standard for speech. |
| Filter number | 32 | Mel filter banks. 32 balances resolution and compute. |
| FFT length | 256 | 16 ms FFT at 16 kHz. Good resolution. |
| Noise floor | -52 dB | Silences very quiet frames. Adjust if your INMP441 is noisier. |
| Pre-emphasis | 0.98 | Boosts high frequencies. Standard for speech. |

Check the **On-device performance** panel. Target for your ESP32 at 160 MHz:

- DSP time (MFCC): < 60 ms per 1 s window
- Peak RAM (MFCC features): < 5 KB

If the panel estimates these values for a Cortex-M4F at 80 MHz, your LX6 at 160 MHz will be roughly 2× faster (no SIMD, but higher clock).

Click **Save parameters**, then **Generate features**. Wait for completion.

---

## 10. EON Tuner — find the optimal config

The EON Tuner searches MFCC parameters and NN architecture together to find the best accuracy within your RAM and flash budget. Run it before manual training.

1. Go to **Experiments → EON Tuner**.
2. Click **New run**.
3. **Target device**: select your custom board or the closest proxy (Cortex-M4F at 80 MHz).
4. **Optimization objectives** (in priority order):
   1. Accuracy (highest priority)
   2. RAM usage
   3. Latency
5. **Search space**: leave defaults. Make sure MFCC and MFE are both enabled as processing block options.
6. Set compute time to **1 hour** for a thorough search.
7. Click **Start run**.

While the tuner runs, read sections 11 and 12 so you understand what you will evaluate.

**Reading tuner results**

The results table shows each trial with accuracy, RAM, flash and latency. Find the trial that:
- Has the highest test accuracy
- Has RAM (peak arena) under 40 KB — this is the single largest allocation, and must fit in internal DRAM
- Has flash under 80 KB

Click the three dots on that trial and **Add to experiments**. Then click **Set as active impulse** to use it for deployment.

If no trial is both accurate and within RAM budget, re-run with narrower search space: fix MFCC coefficients to 13 and restrict NN to 1D Conv only.

---

## 11. Neural network training

If you skip the EON Tuner or want to tune manually, go to **Classifier**.

**Architecture** (starting point — EON Tuner may find better)

```
Input: MFCC features (13 coefficients × 101 frames = 1313 floats)
  ↓
1D Conv (8 filters, kernel 3, ReLU)
  ↓
Dropout (0.25)
  ↓
1D Conv (16 filters, kernel 3, ReLU)
  ↓
Flatten
  ↓
Dense (32, ReLU)
  ↓
Dropout (0.25)
  ↓
Dense (3, Softmax)   → [hey_hotpin, noise, unknown]
```

**Training settings**

| Setting | Value |
|---|---|
| Number of training cycles | 100 |
| Learning rate | 0.005 |
| Validation set size | 20% |
| Minimum confidence rating | 0.80 |
| Data augmentation | **ON** |

**Data augmentation** randomly applies noise addition, time masking and frequency masking during each training epoch. This is critical for a wake word that must work in real rooms. Do not disable it.

**int8 quantisation**

After training, enable **Quantization (int8)** in the training panel or in the Deployment build. This halves the model size and significantly reduces inference time on MCUs at the cost of 1–2% accuracy. Always use int8 for deployment on the ESP32.

**Expected training performance targets**

| Metric | Target |
|---|---|
| Training accuracy | > 90% |
| Validation accuracy | > 85% |
| `hey_hotpin` F1 | > 90% |
| `unknown` recall | > 90% |
| `noise` recall | > 95% |

If validation accuracy is 5+ points below training accuracy, the model is overfitting. Add more `unknown` and noise data, or increase dropout. Do not simply reduce training cycles.

---

## 12. Model testing

Go to **Model testing**. Select all samples. Click **Classify selected**.

**Accept criteria before deploying**

- Test set accuracy > 85%
- False accept rate on `unknown` and `noise` < 5% each
- No `hey_hotpin` sample classified as `noise` at > 10% rate (noise misclassification is the worst failure mode — the device won't even know someone spoke)

**Inspecting failures**

Click the three dots → **Show classification** on any misclassified sample. Listen to the audio. Common causes:

- Phrase was cut off → sample window did not centre the phrase. Add more data.
- Sample is too quiet → mic gain or noise floor issue. Adjust MFCC noise floor.
- Similar word classified as keyword → add more `unknown` samples with those words.
- Keyword classified as unknown → your `unknown` dataset is too large relative to `hey_hotpin`. Balance the classes.

Move misclassified training samples back to the training set after fixing the label (not the test set — this would bias your evaluation).

---

## 13. Deployment — C++ library for ESP-IDF

1. Go to **Deployment**.
2. Select **C++ library**.
3. Enable **Enable EON Compiler** (generates static C++ instead of TFLM interpreter — lower RAM, faster inference).
4. Enable **Quantized (int8)**.
5. Click **Build**. Download the ZIP.
6. Unzip into your ESP-IDF project:

```
hotpin_esp32_firmware/
└── components/
    └── edge-impulse-wake/
        ├── edge-impulse-sdk/
        ├── model-parameters/
        ├── tflite-model/
        ├── CMakeLists.txt          ← create this (see below)
        └── ei_classifier_config.h  ← edit this (see below)
```

**`components/edge-impulse-wake/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)

# Collect all source files from the exported library
file(GLOB_RECURSE EI_SOURCES
    "edge-impulse-sdk/dsp/*.cpp"
    "edge-impulse-sdk/dsp/kissfft/*.cpp"
    "edge-impulse-sdk/dsp/dct/*.cpp"
    "edge-impulse-sdk/classifier/*.cpp"
    "tflite-model/*.cpp"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/*.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/*.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/memory_planner/*.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/*.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/internal/*.cc"
    "edge-impulse-sdk/tensorflow/lite/core/api/*.cc"
    "edge-impulse-sdk/tensorflow/lite/c/common.c"
)

idf_component_register(
    SRCS ${EI_SOURCES}
    INCLUDE_DIRS
        "."
        "edge-impulse-sdk"
        "edge-impulse-sdk/classifier"
        "edge-impulse-sdk/porting"
        "model-parameters"
        "tflite-model"
    REQUIRES
        freertos
        driver
        esp_system
        heap
)

# CRITICAL: disable ESP-NN SIMD acceleration
# The classic ESP32 (LX6) has no vector instructions.
# ESP-NN requires LX7 (ESP32-S3). Without this flag the build will fail
# or produce incorrect results.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0
    TF_LITE_DISABLE_X86_NEON=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu++14
    -Wno-strict-aliasing
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)
```

**`components/edge-impulse-wake/ei_classifier_config.h`** (if not already present, create it)

```c
#ifndef EI_CLASSIFIER_CONFIG_H
#define EI_CLASSIFIER_CONFIG_H

// Disable ESP-NN — classic ESP32 has no SIMD (LX6, not LX7)
#define EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN 0

#endif // EI_CLASSIFIER_CONFIG_H
```

---

## 14. ESP-IDF integration

Create `main/wake_word.c` in your HOTPIN firmware.

```c
/**
 * @file wake_word.c
 * @brief Edge Impulse "Hey Hotpin" wake word detector
 *
 * Runs on Core 1, priority 5 (below STT/TTS at 7, state_mgr at 10).
 * Uses a 4-of-5 vote filter to reduce false accepts.
 */

#include "wake_word.h"
#include "audio_driver.h"
#include "event_dispatcher.h"
#include "system_events.h"
#include "config.h"

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "WAKE_WORD";

// ── Tuneable constants ────────────────────────────────────────────────────────

// Inference window: 1000 ms @ 16 kHz = 16000 samples
#define WAKE_WORD_WINDOW_SAMPLES     EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE

// Stride: 250 ms = 4000 samples.  Model runs 4×/second.
#define WAKE_WORD_STRIDE_SAMPLES     4000

// Classification threshold: fire only when confidence > this value
#define WAKE_WORD_THRESHOLD          0.82f

// Vote filter: require N of WINDOW consecutive slices above threshold
#define VOTE_REQUIRED                2
#define VOTE_WINDOW                  5

// Label index for hey_hotpin in the model (alphabetical order in EI Studio)
// Verify this against ei_classifier_inferencing_categories[] at boot
#define LABEL_HEY_HOTPIN             0   // adjust if Studio shows different order

// ── Internal state ────────────────────────────────────────────────────────────

static float    s_audio_buf[WAKE_WORD_WINDOW_SAMPLES];
static uint8_t  s_vote_history[VOTE_WINDOW] = {0};
static uint8_t  s_vote_head = 0;

static volatile bool s_enabled  = false;
static volatile bool s_paused   = false;
static TaskHandle_t  s_task     = NULL;

// ── Signal callback required by EI SDK ───────────────────────────────────────

static int wake_word_get_signal_data(size_t offset, size_t length, float *out_ptr)
{
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = s_audio_buf[offset + i];
    }
    return EIDSP_OK;
}

// ── Vote filter ───────────────────────────────────────────────────────────────

static bool vote_filter_update(bool triggered)
{
    s_vote_history[s_vote_head] = triggered ? 1 : 0;
    s_vote_head = (s_vote_head + 1) % VOTE_WINDOW;

    uint8_t votes = 0;
    for (int i = 0; i < VOTE_WINDOW; i++) {
        votes += s_vote_history[i];
    }
    return (votes >= VOTE_REQUIRED);
}

// ── Inference ─────────────────────────────────────────────────────────────────

static void run_inference(void)
{
    signal_t signal;
    signal.total_length = WAKE_WORD_WINDOW_SAMPLES;
    signal.get_data     = wake_word_get_signal_data;

    ei_impulse_result_t result;
    int64_t t0 = esp_timer_get_time();
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;

    if (err != EI_IMPULSE_OK) {
        ESP_LOGE(TAG, "Classifier error %d", err);
        return;
    }

    float confidence = result.classification[LABEL_HEY_HOTPIN].value;
    ESP_LOGD(TAG, "hey_hotpin=%.3f  DSP=%d ms  NN=%d ms  total=%lld ms",
             confidence,
             result.timing.dsp,
             result.timing.classification,
             elapsed_ms);

    bool triggered = (confidence >= WAKE_WORD_THRESHOLD);
    bool fired     = vote_filter_update(triggered);

    if (fired) {
        ESP_LOGI(TAG, "WAKE WORD DETECTED (confidence=%.3f)", confidence);

        // Clear vote history to prevent immediate re-fire
        for (int i = 0; i < VOTE_WINDOW; i++) s_vote_history[i] = 0;

        system_event_t evt = {
            .type         = SYSTEM_EVENT_WAKE_WORD,
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        };
        event_dispatcher_post(&evt, pdMS_TO_TICKS(10));
    }
}

// ── Main task ─────────────────────────────────────────────────────────────────

static void wake_word_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Wake word task started on core %d", xPortGetCoreID());

    // Log label order so you can verify LABEL_HEY_HOTPIN is correct
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        ESP_LOGI(TAG, "Label[%u] = %s", (unsigned)i,
                 ei_classifier_inferencing_categories[i]);
    }

    // Circular buffer position within the window
    size_t window_pos = 0;
    int16_t raw_buf[WAKE_WORD_STRIDE_SAMPLES];

    // Pre-fill audio window with silence
    memset(s_audio_buf, 0, sizeof(s_audio_buf));

    while (true) {
        if (!s_enabled || s_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Read one stride of audio from I2S mic
        size_t bytes_read = 0;
        esp_err_t ret = audio_driver_read(
            (uint8_t *)raw_buf,
            sizeof(raw_buf),
            &bytes_read,
            200  // ms timeout
        );

        if (ret != ESP_OK || bytes_read != sizeof(raw_buf)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Convert int16 PCM → float and slide into window
        for (size_t i = 0; i < WAKE_WORD_STRIDE_SAMPLES; i++) {
            s_audio_buf[(window_pos + i) % WAKE_WORD_WINDOW_SAMPLES] =
                (float)raw_buf[i] / 32768.0f;
        }
        window_pos = (window_pos + WAKE_WORD_STRIDE_SAMPLES)
                     % WAKE_WORD_WINDOW_SAMPLES;

        // Run inference once per stride
        run_inference();

        // Yield so the watchdog can run.
        // Inference should take 20–60 ms; this adds headroom.
        vTaskDelay(1);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t wake_word_init(void)
{
    ESP_LOGI(TAG, "Initialising wake word detector");
    ESP_LOGI(TAG, "Window: %d samples · Stride: %d samples",
             WAKE_WORD_WINDOW_SAMPLES, WAKE_WORD_STRIDE_SAMPLES);

    s_enabled = false;
    s_paused  = false;
    memset(s_vote_history, 0, sizeof(s_vote_history));

    return ESP_OK;
}

esp_err_t wake_word_start(void)
{
    if (s_task != NULL) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    s_enabled = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        wake_word_task,
        "wake_word",
        4096,
        NULL,
        5,                  // priority 5 — below STT(7) TTS(7) state_mgr(10)
        &s_task,
        TASK_CORE_CONTROL   // Core 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        s_enabled = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wake word detector started");
    return ESP_OK;
}

void wake_word_pause(void)
{
    s_paused = true;
    ESP_LOGI(TAG, "Paused");
}

void wake_word_resume(void)
{
    // Clear vote history to prevent stale positive from TTS audio
    memset(s_vote_history, 0, sizeof(s_vote_history));
    s_paused = false;
    ESP_LOGI(TAG, "Resumed");
}

void wake_word_stop(void)
{
    s_enabled = false;
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    ESP_LOGI(TAG, "Stopped");
}
```

**`main/include/wake_word.h`**

```c
#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include "esp_err.h"

esp_err_t wake_word_init(void);
esp_err_t wake_word_start(void);
void      wake_word_pause(void);
void      wake_word_resume(void);
void      wake_word_stop(void);

#endif // WAKE_WORD_H
```

**Add `SYSTEM_EVENT_WAKE_WORD` to `system_events.h`**

```c
typedef enum {
    // ... existing events ...
    SYSTEM_EVENT_WAKE_WORD,          // ← add this
} system_event_type_t;
```

**Call from `main.c`**

After `stt_pipeline_init()`:
```c
ESP_LOGI(TAG, "Initialising wake word detector...");
ESP_ERROR_CHECK(wake_word_init());
ESP_ERROR_CHECK(wake_word_start());
```

**Pause during TTS and STT**

In `state_manager.c`, wherever you set state to `VOICE_ACTIVE` (STT starts):
```c
wake_word_pause();
```

Wherever TTS finishes playing and you return to `CAMERA_STANDBY`:
```c
wake_word_resume();
```

---

## 15. Continuous inference with sliding window

The code in section 14 implements continuous sliding-window inference manually. The key points:

- The 1 s audio window (`s_audio_buf`) is kept resident in memory and updated by overwriting the oldest stride each cycle.
- Inference runs on the full 1 s window every 250 ms stride.
- This is equivalent to EI's `run_classifier_continuous()` but gives you direct control over the ring buffer, which is important for coexistence with `audio_driver_read`.

If you want to use EI's built-in continuous mode instead, replace the manual ring buffer with:

```c
run_classifier_init();  // call once at init

// In the task loop, after filling `raw_buf` with one stride of float audio:
run_classifier_continuous(&signal, &result, false, true);
```

The built-in version keeps an internal ring buffer. Note: `run_classifier_continuous` expects the signal to contain only the new stride samples (`WAKE_WORD_STRIDE_SAMPLES`), not the full window.

---

## 16. Vote filter — reducing false accepts

The vote filter in `wake_word.c` keeps a circular history of the last 5 inference results and fires only when 2 or more of the last 5 results exceed the threshold.

| Setting | Effect |
|---|---|
| Threshold = 0.82, votes = 2/5 | Balanced: catches most real phrases, rejects most false positives |
| Threshold = 0.90, votes = 3/5 | More conservative: fewer false accepts, more false rejects |
| Threshold = 0.70, votes = 1/5 | Aggressive: low false rejects, high false accepts |

Start with the balanced setting and adjust based on measured false accept rate per hour in your actual environment. Run the device for 60 minutes with TV audio playing and count how many times it fires. Target < 2 false accepts per hour.

---

## 17. Memory and CPU verification

Run this at boot in your firmware, after `wake_word_start()`, to confirm the budget.

```c
#include "esp_heap_caps.h"
#include "esp_psram.h"

void log_memory_budget(void) {
    size_t internal_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t dma_free         = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t psram_free       = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI("MEM", "Internal free:    %u bytes", (unsigned)internal_free);
    ESP_LOGI("MEM", "Internal largest: %u bytes ← must stay < 147 KB", (unsigned)internal_largest);
    ESP_LOGI("MEM", "DMA free:         %u bytes ← must stay > 32 KB for I2S");
    ESP_LOGI("MEM", "PSRAM free:       %u bytes", (unsigned)psram_free);
}
```

**Pass criteria**

| Metric | Minimum |
|---|---|
| Largest internal block | > 80 KB after wake word init |
| DMA free | > 32 KB at all times |
| Inference time (logged by EI) | DSP + NN < 100 ms per window |
| No `EI_IMPULSE_ERROR` codes | All inference calls return `EI_IMPULSE_OK` |
| No watchdog resets | Zero WDT events over 30-minute soak |

---

## 18. Iteration checklist

Use this after each training run before deploying to the board.

- [ ] Test set accuracy > 85%
- [ ] `hey_hotpin` F1 > 90%
- [ ] No `hey_hotpin` classified as `noise` in test set
- [ ] EON Tuner peak RAM < 40 KB (arena only)
- [ ] EON Tuner flash < 80 KB
- [ ] `EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0` confirmed in build
- [ ] Build succeeds in ESP-IDF 5.4 with no undefined-symbol errors
- [ ] Boot log shows correct label order: `Label[0] = hey_hotpin`
- [ ] Internal largest heap block > 80 KB after `wake_word_start()`
- [ ] Inference time < 100 ms (from ESP timer logs)
- [ ] No watchdog resets over 30-minute soak at 160 MHz
- [ ] < 2 false accepts per hour with TV audio playing
- [ ] Phrase detected reliably from 1 m in a noisy room
- [ ] No self-trigger during TTS playback (verify pause/resume calls)

---

*Sources: Edge Impulse keyword spotting tutorial (https://docs.edgeimpulse.com/tutorials/end-to-end/keyword-spotting), EI TTS data generation guide (https://docs.edgeimpulse.com/tutorials/topics/data/generate-keyword-data-google-tts), EI C++ deployment guide (https://docs.edgeimpulse.com/hardware/deployments/run-cpp), EON Tuner docs (https://docs.edgeimpulse.com/studio/projects/eon-tuner). Retrieved October 2026.*
