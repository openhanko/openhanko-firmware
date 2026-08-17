#pragma once

#include <stdbool.h>
#include <stdint.h>

// Debounced presence button.
//
// Unlike the ESP32 build there is no RTOS here, so there is no claim/release
// arbitration to do: everything runs on one cooperative loop. Whoever calls
// button_pressed() first consumes the press, which is exactly the behaviour the
// mutex was emulating over there.
//
// Must be called regularly for debouncing to work; the config console pumps it
// from inside its own wait loop.

void button_init(void);

// True exactly once per debounced press.
bool button_pressed(void);

// True if the button was already down when button_init() sampled it. Used as
// the manual escape hatch out of pinpad mode, for the cases the automatic
// revert cannot reach.
bool button_held_at_boot(void);
