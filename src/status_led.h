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
  // A pinpad request is outstanding: "touch when ready".
  //
  // Named for what it means rather than for what it looks like. It breathed once
  // and now flashes, and the point of the name is that such a change is a
  // decision about presentation rather than a different state.
  STATUS_LED_WAITING,
  // Something just happened: "I heard you". Both modes can say this.
  //
  // Three quick flashes rather than a steady glow. A press is an event, and a
  // light that simply comes on for a moment is hard to tell from one that is
  // stuck — three deliberate blinks cannot be mistaken for either.
  STATUS_LED_CONFIRM,
  // Steady. Reserved for the factory-reset gesture, where "armed, release to
  // erase" has to be unmistakably different from an acknowledgement.
  STATUS_LED_ARMED,
} status_led_mode_t;

void status_led_init(void);

// Call from the main loop.
void status_led_update(status_led_mode_t mode);
