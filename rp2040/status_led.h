#pragma once

#include <stdbool.h>

// Indicator for "the reader is waiting for you to press the button".
//
// This matters more than it looks. With pinpad PIN entry there is no on-screen
// prompt at all — macOS delegates to the reader and shows nothing — so a device
// with no indicator leaves the user waiting in silence with nothing to react to.
void status_led_init(void);

// Call from the main loop. `waiting` should be true while a pinpad request is
// outstanding.
void status_led_update(bool waiting);
