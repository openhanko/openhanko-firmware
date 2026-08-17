#include <stdio.h>

#include "board_config.h"
#include "button.h"
#include "config_console.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "piv.h"
#include "settings.h"
#include "status_led.h"
#include "storage.h"
#include "trace.h"
#include "tusb.h"
#include "usb_ccid.h"
#include "usb_hid.h"

// The whole authorization story: a debounced button press opens a one-shot
// signing window for the PIV authentication key, then types the dummy PIN so
// the macOS smart-card prompt gets out of the way.
// When the last button press happened. A pinpad request always arrives about a
// second after the press that typed the PIN, so a press that recent is the
// answer to it — the user pressed once and should not have to press again.
static uint32_t last_press_ms;

static uint32_t now_ms(void) {
  return to_ms_since_boot(get_absolute_time());
}

// How long a press stays good for answering a pinpad request.
#define PRESS_ANSWERS_PINPAD_MS 10000

static void handle_press(void) {
  printf("main: button pressed, authorizing PIV and typing the dummy PIN\n");
  config_console_send_line("EVENT PRESS");
  piv_note_user_presence();
  if (!usb_hid_type_line(PIV_DUMMY_PIN)) {
    printf("main: HID interface was not ready; PIN not typed\n");
    config_console_send_line("EVENT PIN_NOT_TYPED");
  }
}

// Gives up on pinpad mode when nothing claims the card.
//
// Exactly one driver owns a card and macOS picks it at insertion from the AID
// alone, so a device in pinpad mode on a Mac without the driver is simply inert
// — no token binds and nothing explains why. Rather than strand the user, the
// card notices that its private AID went unselected and returns to the standard
// AID, where Apple's pivtoken will bind and the device works unaided.
//
// The reboot is the point: the AID is answered during enumeration, so the host
// has to be made to enumerate again. tud_disconnect()/tud_connect() proved
// unreliable here, and a watchdog reset is unambiguous.
static void revert_if_unclaimed(void) {
  if (settings_aid_mode() != AID_MODE_PINPAD) return;

  uint32_t contacted = piv_first_contact_ms();
  if (contacted == 0) return;              // no host yet: a charger, or still enumerating
  if (piv_private_aid_selected()) return;  // our driver is here
  if ((now_ms() - contacted) < AID_REVERT_TIMEOUT_MS) return;

  printf("main: no driver claimed the private AID; reverting to standard\n");
  config_console_send_line("EVENT AID_REVERT standard");
  if (settings_set_aid_mode(AID_MODE_STANDARD)) {
    sleep_ms(50);  // let the console line reach the host
    watchdog_reboot(0, 0, 0);
  }
}

int main(void) {
  stdio_init_all();
  printf("\nsmart-card (rp2040) starting\n");

  button_init();
  settings_init();

  // Escape hatch. A device that has been switched to pinpad mode and then moved
  // to a Mac without the driver recovers on its own, but only once a host talks
  // to it. Holding the button through power-up forces standard mode
  // unconditionally, which covers the cases automation cannot reach.
  if (button_held_at_boot()) {
    printf("main: button held at boot; forcing the standard AID\n");
    settings_set_aid_mode(AID_MODE_STANDARD);
  }
  status_led_init();
  storage_init();
  piv_init();
  config_console_init();
  usb_ccid_start(piv_handle_apdu);

  // No RTOS. Everything runs here: USB, the console, and the button. The only
  // rule is that nothing may block long enough to starve tud_task(), which is
  // why the console pumps USB itself while waiting for a press.
  while (true) {
    tud_task();
    config_console_poll();
    revert_if_unclaimed();

    status_led_update(usb_ccid_pin_pending());

    if (usb_ccid_pin_pending()) {
      // The host is waiting on a pinpad PIN entry. Either the user presses now,
      // or they already pressed a moment ago to satisfy the PIN prompt that
      // triggered this request — both are the same physical act of presence, so
      // one press is enough.
      bool recent = last_press_ms != 0 &&
                    (now_ms() - last_press_ms) < PRESS_ANSWERS_PINPAD_MS;
      if (button_pressed() || recent) {
        printf("main: answering pinpad PIN entry%s\n", recent ? " (recent press)" : "");
        config_console_send_line("EVENT PINPAD_OK");
        last_press_ms = 0;
        piv_note_pin_verified();
        piv_note_user_presence();
        usb_ccid_pin_complete(true);
      } else {
        usb_ccid_pin_tick();
      }
      continue;
    }

    if (button_pressed()) {
      last_press_ms = now_ms();
      handle_press();
    }
  }
}
