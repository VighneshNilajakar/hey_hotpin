# ESP-Waker — "Hey Hotpin" Wake Word on ESP32-CAM

An always-on wake word detector for the AI-Thinker **ESP32-CAM**, running an
Edge Impulse keyword-spotting model over an **INMP441** I2S microphone. When it
hears *"Hey Hotpin"* it fires a callback and flashes the on-board LED.

* Edge Impulse project: <https://studio.edgeimpulse.com/studio/1127922>
* Target: classic ESP32 (Xtensa LX6, no FPU, no SIMD)
* Toolchain: ESP-IDF 5.5.x

Measured on the bench at 1–2 m in a quiet room: real hits at **0.95–0.996**,
ordinary speech peaking near **0.50**, a quiet room under **0.05**, and **zero
false accepts** across several hundred inferences.

---

## 1. Hardware

| Part | Notes |
|---|---|
| AI-Thinker ESP32-CAM | 4 MB flash. PSRAM **disabled** (see §7) |
| INMP441 I2S MEMS mic | 3V3 only |
| USB-TTL adapter | FTDI FT232R or CH340 |

### Wiring — INMP441

| INMP441 | ESP32-CAM | Note |
|---|---|---|
| `SCK` | **GPIO14** | I2S bit clock |
| `WS` | **GPIO15** | I2S word select |
| `SD` | **GPIO13** | I2S data in |
| `L/R` | **GND** | selects the left channel |
| `VDD` | **3V3** | never 5 V |
| `GND` | **GND** | |

Common ground between the mic and the board is required.

### Wiring — USB-TTL

| FTDI | ESP32-CAM |
|---|---|
| TXD | `U0T` (GPIO1) |
| RXD | `U0R` (GPIO3) |
| GND | GND |
| 5 V | 5 V |
| DTR | `IO0` *(optional, for auto-download)* |
| RTS | `EN` *(optional)* |

Without DTR→IO0 you must bridge `IO0` to GND by hand before every flash.

---

## 2. ⚠️ Read this before wiring anything

On an ESP32-CAM the SPI flash **chip is soldered to GPIOs 2, 4, 12, 13, 14 and 15.**
Using those pins for the microphone works only because of two mitigations this
project applies:

| Mitigation | Why |
|---|---|
| **Flash runs in DIO mode**, not QIO | Frees GPIO4/SD1 and reduces the lines in contention |
| **NVS is never initialised** | It is the only thing in this firmware that *writes* flash |

Consequences you must respect:

* Do **not** re-enable QIO flash mode.
* Do **not** add code that writes flash — NVS, `esp_camera`, OTA, coredumps.
  The mic clock and the flash controller will fight over the same pads and you
  will get brownout resets or a silent board.
* GPIO4 also drives the on-board flash LED. That is safe in DIO mode only.

If you later move the microphone to `18 / 19 / 23` (free once the camera is
removed) all of these restrictions disappear.

### GPIOs to avoid entirely

| Pin | Reason |
|---|---|
| 2, 4, 12, 13, 14, 15 | SPI flash bus |
| 1, 3 | UART0 — serial console and flashing |
| 0 | boot strap (IO0) |
| 5 | boot strap (CS0) |
| 16 | PSRAM |
| 34, 35, 36, 39 | input only — cannot drive a clock |
| 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 | free once the camera is removed |

---

## 3. Prerequisites

1. **ESP-IDF 5.5.x** installed via the Espressif installer
   (Windows: `C:\Espressif\frameworks\esp-idf-v5.5.3`).
2. `python` available with `pyserial`.

> **Windows tip:** if your default `python` is newer than the one the IDF virtual
> environment was built against, `export.ps1` fails with *"ESP-IDF Python
> virtual environment not found"*. Use the bundled `idf.bat`, which pins the
> correct environment for you.

---

## 4. Build, flash, monitor

```bat
idf.bat build
idf.bat -p COM13 flash
idf.bat -p COM13 monitor
```

Or with a normal ESP-IDF shell:

```bash
idf.py build
idf.py -p COM13 flash monitor
```

### Serial monitor without ESP-IDF

```bash
python tools/monitor.py -p COM13 --reset
```

`--reset` pulses the adapter's RTS line into `EN` (needs RTS wired).

---

## 5. Configuration

Options live under **Wake word detector** and **INMP441 microphone**
(`idf.bat menuconfig`). They are also mirrored in `sdkconfig.defaults`.

| Option | Default | Meaning |
|---|---|---|
| `WAKE_WORD_THRESHOLD_PCT` | `95` | Fire when `hey_hotpin` reaches this. Raise to cut false accepts, lower to catch quieter attempts. |
| `WAKE_WORD_SELFTEST` | `y` | Run synthetic inferences at boot and log MFCC/NN timing. |
| `MIC_BCLK_GPIO` / `MIC_WS_GPIO` / `MIC_DIN_GPIO` | `14` / `15` / `13` | See §2. |
| `MIC_GAIN_SHIFT` | `11` | Lower = louder. Watch `audio peak` in the log. |
| `STATUS_LED_GPIO` | `4` | `-1` disables. |
| `STATUS_LED_ON_MS` | `250` | LED duration after a trigger. |

### Tuning the microphone gain

Every 12 windows the firmware logs the audio level and the peak classifier score:

```
WAKE_WORD: PEAK 12 windows: hey_hotpin 99.6% (thr 95%) | audio peak 7595 rms 822 of 32768
```

* `audio peak` pinned at **32768** → clipping, raise `MIC_GAIN_SHIFT` by 1
* `audio peak` under **3000** while speaking → too quiet, lower it by 1

Target is roughly 8000–25000 for conversational speech.

---

## 6. Wiring up your own actions

`wake_word_start()` takes a callback. Put your logic in `main.c`:

```c
static void on_wake_word(float confidence)
{
    ESP_LOGW(TAG, "wake word fired at %.3f", (double)confidence);
    /* start the camera, post an event, begin STT, ... */
}
```

Call `wake_word_pause()` while your own audio output is playing, and
`wake_word_resume()` when it finishes, otherwise the device will hear itself.

---

## 7. Measured performance

ESP32-D0WDQ6-V3 @ 160 MHz, DIO flash, `MIC_GAIN_SHIFT=11`:

| Metric | Value |
|---|---|
| MFCC DSP | ~96 ms per 1 s window |
| Neural network (int8, EON) | ~27 ms |
| Total per inference | ~123 ms |
| Inference cadence | ~240 ms (stride 4000 samples) |
| Largest internal heap block | 102 KB *(pass mark: > 80 KB)* |
| Free DMA heap | 201 KB *(pass mark: > 32 KB)* |

**The DSP exceeds the guide's < 60 ms target.** The MFCC is float-heavy and the
LX6 has no FPU, so the remaining cost is inherent to kissfft on this chip.
Closing the gap needs ESP-DSP, which requires an ESP32-S3 or ESP32-P4.

Two optimisations are already applied to the vendored SDK and are load-bearing:

1. **`numpy.hpp` — `software_rfft()` caches the `kiss_fftr_cfg`.**
   Upstream rebuilds the twiddle factors with double-precision `sin`/`cos` once
   per analysis frame (49× per window). On a chip without an FPU that alone cost
   **771 ms**. Caching it: **~30 ms**.
2. **`speechpy/feature.hpp` — hoisted the mel-bank divisions.**
   The rising/falling slopes depended only on the bin indices, which are fixed
   per signal, but the division was inside the per-bin loop (~400 k software
   float divisions per window). Taking the reciprocal once per filter saves
   ~10 ms.

Both edits are marked with comments explaining the reasoning. Re-applying a fresh
Edge Impulse export will revert them.

---

## 8. Project layout

```
.
├── CMakeLists.txt              ESP-IDF project root
├── partitions.csv              nvs + phy_init + 1 MB factory app
├── sdkconfig.defaults          reproducible build defaults
├── idf.bat                     ESP-IDF wrapper (see §3)
├── components/
│   └── edge-impulse-wake/
│       └── CMakeLists.txt      compiles only the sources the graph needs
├── main/
│   ├── main.c                  boot, self test, memory budget
│   ├── wake_word.cpp           sliding window, vote filter, LED
│   ├── audio_driver.c          INMP441 over I2S
│   ├── Kconfig.projbuild       tunables
│   └── include/                board_pins.h, wake_word.h, audio_driver.h
├── tools/
│   └── monitor.py              standalone serial capture
├── model-parameters/           generated by Edge Impulse
├── tflite-model/               generated by Edge Impulse (EON-compiled int8)
├── edge-impulse-sdk/           vendored Edge Impulse SDK
└── hey-hotpin-wake-word-training.md   original training/deployment guide
```

`edge-impulse-sdk/porting/` is trimmed to the Espressif port only, and unused
platform trees were removed. CMSIS and `third_party/flatbuffers` **are**
required (pulled in by `micro_interpreter.h`) and are kept.

---

## 9. Troubleshooting

**Board is silent after flashing / `Failed to connect to ESP32`**
The board is booting normally instead of entering the download bootloader.
Bridge `IO0` to GND, run the flash, then remove the bridge and press RST.

**`E BOD: Brownout detector was triggered` in a loop**
The 3 V rail is sagging as the SoC ramps to 160 MHz. It dies in `heap_init`,
before any application code runs. Add **100–470 µF** across 3V3 and GND, or
power the board from a separate 5 V supply while USB carries only data.

**Everything reads `noise 99.6%` / mic is dead**
Check the wiring in §1 and that `L/R` is tied to GND. `first stride:` in the
boot log should show non-zero samples.

**LED is inverted**
`STATUS_LED_ACTIVE_LOW` in `main/include/board_pins.h` — this board is
active **high**.

**`ESP-NN` / `ESP-DSP` compile errors**
`components/edge-impulse-wake/CMakeLists.txt` must define both
`EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0` and `EIDSP_USE_ESP_DSP=0`. Both are
SIMD-only kernels for the S3/P4 and are wrong for the classic ESP32.

---

## 10. Regenerating the model

The Edge Impulse deployment is committed, so a normal build needs nothing extra.
To update it:

1. Open <https://studio.edgeimpulse.com/studio/1127922>
2. **Deployment → C++ library**
3. **Enable EON Compiler** and **Quantized (int8)**
4. Download and replace `edge-impulse-sdk/`, `model-parameters/`, `tflite-model/`
5. Re-apply the two DSP optimisations in §7 — they do not survive a re-export
6. `idf.bat build`

The label order (`hey_hotpin`, `noise`, `unknown`) is verified against the model
at boot and logged by `wake_word_init()`; a mismatch is reported as an error.

---

## 11. Licences

The vendored Edge Impulse SDK carries its own licence terms in
`edge-impulse-sdk/LICENSE` and `edge-impulse-sdk/LICENSE.3-clause-bsd-clear`.
Review those before commercial use — the generated model files are subject to
your Edge Impulse subscription terms.

Application code in `main/` and `components/` is yours to license as you see fit.
