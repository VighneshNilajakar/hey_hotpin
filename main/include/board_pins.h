/*
 * Board pin map for AI-Thinker ESP32-CAM + INMP441 I2S microphone.
 *
 * !! CURRENT BUILD USES THE SPI FLASH BUS ON PURPOSE - EXPECT PROBLEMS !!
 *
 * The current Kconfig values are BCLK=GPIO14, WS=GPIO15, DIN=GPIO13, which are
 * SD_CLK, SD_CMD and SD_DATA3 of the on-board SPI flash. The I2S peripheral and
 * the flash controller drive the same pads, so any flash access while capturing
 * corrupts the audio clock and any audio-driven state corrupts flash. There is
 * no flash mode that frees GPIO14 or GPIO15 - they are used in DIO as well.
 *
 * Measured on this board already:
 *   - GPIO4 (flash SD1) -> brownout detector triggered, then a silent board
 *   - GPIO14/15/2       -> zero serial output in every reset variant
 *
 * If this build misbehaves, the fallback on a stock ESP32-CAM-MB header is
 * SCK=GPIO5, WS=GPIO3, SD=GPIO1, which costs the UART console.
 *
 * GPIOs that must never be used regardless:
 *   2, 4, 12, 13, 14, 15  SPI flash bus
 *   1, 3                   UART0 - serial console and flashing
 *   16                     PSRAM (WROVER-B)
 *   0                      boot strap (IO0)
 *   5                      boot strap (CS0)
 *   34, 35, 36, 39         input only - cannot drive a clock
 *
 * 18, 19, 21, 22, 25, 26, 27, 32, 33 are safe once the camera is removed.
 */

#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "sdkconfig.h"

#if defined(CONFIG_MIC_BCLK_GPIO)
#define MIC_BCLK_GPIO   CONFIG_MIC_BCLK_GPIO
#else
#define MIC_BCLK_GPIO   14
#endif

#if defined(CONFIG_MIC_WS_GPIO)
#define MIC_WS_GPIO     CONFIG_MIC_WS_GPIO
#else
#define MIC_WS_GPIO     15
#endif

#if defined(CONFIG_MIC_DIN_GPIO)
#define MIC_DIN_GPIO    CONFIG_MIC_DIN_GPIO
#else
#define MIC_DIN_GPIO    13
#endif

/* INMP441 L/R pin: 1 = tie to GND (LEFT), 0 = tie to 3V3 (RIGHT). */
#define MIC_CHANNEL_LEFT    1

/*
 * Right-shift applied to the filtered 32-bit I2S slot to land on int16.
 * The mic's 24-bit payload sits in bits 31..8, so the full-scale slot is about
 * +-2^31 and a >>14 clips at roughly 25% of microphone full scale. Normal
 * speech at a metre lands near 2% of full scale, which is why the detector only
 * fired when the user was right at the mic. Lower the number = more gain.
 */
#ifndef CONFIG_MIC_GAIN_SHIFT
#define CONFIG_MIC_GAIN_SHIFT 11
#endif
#define MIC_GAIN_SHIFT      CONFIG_MIC_GAIN_SHIFT

#if defined(CONFIG_STATUS_LED_GPIO)
#define STATUS_LED_GPIO CONFIG_STATUS_LED_GPIO
#else
#define STATUS_LED_GPIO 4
#endif

/* The AI-Thinker ESP32-CAM LED on GPIO4 is ACTIVE HIGH on this board: the
 * firmware shipped with level=1 for "off" and the LED stayed lit the whole
 * time, which is how the polarity was confirmed. Flip this back to 1 if you
 * swap in a board wired through an NPN inverter.
 *
 * GPIO4 is also SPI flash SD1. The flash controller only drives that line in
 * QIO mode; this build runs DIO, so the pin is free at runtime. Do not switch
 * back to QIO while this LED is enabled. */
#define STATUS_LED_ACTIVE_LOW 0

/* How long the LED stays lit after a trigger. */
#ifndef CONFIG_STATUS_LED_ON_MS
#define CONFIG_STATUS_LED_ON_MS   250
#endif
#define STATUS_LED_ON_MS    CONFIG_STATUS_LED_ON_MS

#endif /* BOARD_PINS_H */
