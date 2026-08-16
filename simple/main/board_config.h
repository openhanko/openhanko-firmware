#pragma once

// Presence button.
//
// Any free GPIO works. GPIO0 is the BOOT button on virtually every ESP32-S3
// dev board, so the proof of concept runs with no extra wiring at all.
//
//   momentary switch to GND   -> ACTIVE_LEVEL 0, PULL_UP 1
//   TTP223-style touch module -> ACTIVE_LEVEL 1, PULL_UP 0
//
// Holding GPIO0 down while the board resets puts it into firmware download
// mode. Move the button to another pin once you stop using the BOOT button.
#define BUTTON_GPIO 0
#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_PULL_UP 1

#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_SAMPLES 3
#define BUTTON_MIN_INTERVAL_MS 400

// Addressable RGB status LED (WS2812).
//
// GPIO48 on ESP32-S3-DevKitC-1 v1.0 / DevKitM-1 / Freenove; GPIO38 on
// DevKitC-1 v1.1; GPIO21 on the Waveshare S3-Zero. Set to -1 if the board has
// no RGB LED.
//
// The firmware drives it to off at boot. A WS2812 latches whatever it was last
// sent, so a board left green by earlier firmware stays green until something
// explicitly clears it.
#define STATUS_LED_GPIO 48

// Peak brightness while breathing, out of 255. These LEDs are painfully bright
// at full scale.
#define STATUS_LED_BRIGHTNESS 64
#define STATUS_LED_COLOR_R 0
#define STATUS_LED_COLOR_G 1
#define STATUS_LED_COLOR_B 0
#define STATUS_LED_BREATHE_MS 2000

// The PIV PIN macOS insists on collecting. It is not a secret and it is not
// your account password: the real authorization gate is the button press that
// opens the signing window inside piv.c.
#define PIV_DUMMY_PIN "000000"
