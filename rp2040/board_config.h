#pragma once

// Presence button.
//
// Set BUTTON_USE_BOOTSEL to 1 to use the BOOTSEL button instead of a wired one,
// which makes the board work with no extra hardware — the RP2040 equivalent of
// borrowing GPIO0 on the ESP32-S3.
//
// It is not free: reading BOOTSEL means overriding the flash chip-select and
// disabling interrupts for tens of microseconds, because the pin is on the QSPI
// bus rather than the GPIO bank. Sampling is rate-limited to
// BUTTON_SAMPLE_MS to keep that off USB's back. Good for bench work; wire a
// real button for anything you intend to keep.
#define BUTTON_USE_BOOTSEL 0

//   momentary switch to GND   -> ACTIVE_LEVEL 0, PULL_UP 1
//   TTP223-style touch module -> ACTIVE_LEVEL 1, PULL_UP 0
//
// GPIO10 with the internal pull-up: wire the switch between GP10 and any GND
// pin. No external resistor needed.
#define BUTTON_GPIO 10
#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_PULL_UP 1

// How often the pin is actually sampled. Matters mostly for BOOTSEL, where each
// sample costs an interrupt blackout.
#define BUTTON_SAMPLE_MS 20

// Time-based debounce: the main loop runs at whatever rate USB work allows, so
// counting samples would drift with load.
#define BUTTON_DEBOUNCE_MS 40
#define BUTTON_MIN_INTERVAL_MS 400

// Which AID a factory-fresh device answers. Runtime state after that — see
// settings.h — so one firmware image serves both workflows with no reflash.
//
// Standard is the default deliberately: an unopened box then works on any Mac
// with nothing installed. Pinpad is an opt-in upgrade the driver's installer
// switches the device into, and which the device abandons on its own if
// nothing claims it.
#define PIV_DEFAULT_AID_MODE 0  /* AID_MODE_STANDARD */

// How long after the host first talks to the card to wait for our driver to
// claim it before giving up and reverting to the standard AID.
//
// macOS probes AIDs within about two seconds of enumeration, so this is
// generous. It only ever elapses on a Mac where the driver is absent, and the
// user sees the device work a moment after plugging in rather than not at all.
#define AID_REVERT_TIMEOUT_MS 10000

// Addressable RGB indicator (WS2812) on GP16. Driven by PIO, since the protocol
// needs sub-microsecond pulse widths the CPU cannot hit reliably alongside USB.
// Set to -1 to disable.
//
// It breathes only while the reader is waiting for a press. With pinpad PIN
// entry macOS shows no prompt at all, so without this the device gives the user
// nothing to react to.
#define STATUS_LED_GPIO 16
#define STATUS_LED_BRIGHTNESS 72
// Colour weights, multiplied by the brightness ramp.
#define STATUS_LED_COLOR_R 0
#define STATUS_LED_COLOR_G 1
#define STATUS_LED_COLOR_B 0
#define STATUS_LED_BREATHE_MS 1600

// The PIV PIN macOS insists on collecting. Not a secret, and not your account
// password: the button press is the real gate.
#define PIV_DUMMY_PIN "000000"
