#pragma once

typedef enum {
  STATUS_LED_OFF = 0,
  STATUS_LED_BREATHE,   // waiting for a button press
  STATUS_LED_SOLID,     // brief confirmation
} status_led_mode_t;

// Drives the board's WS2812 to off at boot, then follows whatever mode the
// supplied callback reports.
//
// The callback is polled from the LED task; it must be cheap and safe to call
// from another task.
void status_led_init(status_led_mode_t (*mode)(void));
