#pragma once

#include <stdbool.h>
#include <stdint.h>

// Debounced presence button.
//
// There is no RTOS here, so there is no claim/release arbitration to do:
// everything runs on one cooperative loop. Whoever calls button_pressed() first
// consumes the press.
//
// Must be called regularly for debouncing to work; the config console pumps it
// from inside its own wait loop.

void button_init(void);

// True exactly once per debounced press.
bool button_pressed(void);

// True if the button was already down when button_init() sampled it.
bool button_held_at_boot(void);

// The current debounced level. button_pressed() reports edges and consumes
// them, which is wrong for a gesture measured by how long the button is held.
bool button_is_down(void);
