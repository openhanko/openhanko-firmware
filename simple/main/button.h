#pragma once

#include <stdbool.h>
#include <stdint.h>

// Debounced presence button.
//
// A single background task samples the GPIO and publishes at most one pending
// press. Presses are delivered to whoever holds the claim, so the config
// console can take over the button for the duration of a setup command without
// the presence task stealing the press and typing a PIN into whatever happens
// to be focused.

void button_init(void);

// Take exclusive ownership of button presses. Returns false on timeout.
bool button_claim(uint32_t timeout_ms);
void button_release(void);

// Discard a press that arrived before the caller started paying attention.
void button_flush(void);

// Wait for the next press. Only meaningful while holding the claim.
bool button_wait_press(uint32_t timeout_ms);
