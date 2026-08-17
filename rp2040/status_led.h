#pragma once

#include <stdbool.h>

// The device's only output.
//
// Two jobs, and only one of them is available in both modes. In pinpad mode
// macOS shows no prompt at all — it delegates to the reader — so breathing is
// the entire invitation to act, and without it the user waits in silence.
//
// In standard mode the card is told nothing until a PIN has already been
// submitted, so it cannot invite anything: there is no event to light up on.
// What it can still do is acknowledge. A button that swallows presses without
// a flicker reads as broken, especially when the PIN it typed lands in a window
// the user is not looking at.
typedef enum {
  STATUS_LED_OFF = 0,
  // A pinpad request is outstanding: "press when ready".
  STATUS_LED_BREATHE,
  // Something just happened: "I heard you". Both modes can say this.
  STATUS_LED_CONFIRM,
} status_led_mode_t;

void status_led_init(void);

// Call from the main loop.
void status_led_update(status_led_mode_t mode);
