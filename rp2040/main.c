#include <stdio.h>

#include "board_config.h"
#include "button.h"
#include "config_console.h"
#include "fingerprint.h"
#include "identity.h"
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

// How long the indicator acknowledges a press.
//
// Long enough to register as deliberate rather than as a glitch, short enough
// not to be mistaken for the breathing invitation. In standard mode this is the
// only feedback the device can give: it is never told that macOS wants
// anything, so it can confirm what it did but never invite.
#define CONFIRM_MS 600

static uint32_t confirm_until_ms;

static status_led_mode_t led_mode(void) {
  // The invitation outranks the acknowledgement: if macOS is waiting on a
  // pinpad entry, saying "press" matters more than "I heard the last one".
  if (usb_ccid_pin_pending()) return STATUS_LED_BREATHE;
  if (confirm_until_ms != 0 && (int32_t)(now_ms() - confirm_until_ms) < 0) {
    return STATUS_LED_CONFIRM;
  }
  // Holds through the signature that follows a press, so the light covers the
  // whole operation rather than going dark in the middle of it.
  if (piv_recent_signature()) return STATUS_LED_CONFIRM;
  return STATUS_LED_OFF;
}

static void handle_press(void) {
  confirm_until_ms = now_ms() + CONFIRM_MS;
  printf("main: button pressed, authorizing PIV and typing the dummy PIN\n");
  config_console_send_line("EVENT PRESS");
  piv_note_user_presence();
  if (!usb_hid_type_line(PIV_DUMMY_PIN)) {
    printf("main: HID interface was not ready; PIN not typed\n");
    config_console_send_line("EVENT PIN_NOT_TYPED");
  }
}

// Erases the device on a long press held through power-up.
//
// The only destructive action available without a host, so it is deliberately
// awkward: ten seconds, and the *release* is what commits. A device wedged
// against something in a bag can hold a button indefinitely but cannot let go,
// and anyone who started this by accident can keep holding, or let go early,
// and nothing happens.
//
// The light is the entire interface. With no screen, an accelerating blink is
// the only warning available that something irreversible is approaching, and
// going solid is the only way to say "now, if you mean it".
#define RESET_ARM_MS 6000

static void factory_reset_gesture(void) {
  if (!button_held_at_boot()) return;

  uint32_t started = now_ms();
  uint32_t last = started;
  uint32_t phase = 0;      // milliseconds into the current half-cycle
  bool lit = false;
  bool armed = false;

  while (button_is_down()) {
    uint32_t now = now_ms();
    uint32_t step = now - last;
    last = now;
    uint32_t held = now - started;

    if (held >= RESET_ARM_MS) {
      armed = true;
      status_led_update(STATUS_LED_ARMED);
      sleep_ms(5);
      continue;
    }

    // Half-period falls from 250 ms to 50 ms as the deadline approaches.
    //
    // Accumulated rather than derived from the clock. Dividing absolute time by
    // a shrinking period moves the phase discontinuously, so the light stutters
    // instead of accelerating and never reads as settling — which made the
    // moment it went solid impossible to judge.
    uint32_t half = (500 - (held * 400) / RESET_ARM_MS) / 2;
    phase += step;
    if (phase >= half) {
      phase = 0;
      lit = !lit;
    }
    status_led_update(lit ? STATUS_LED_ARMED : STATUS_LED_OFF);
    sleep_ms(5);
  }

  status_led_update(STATUS_LED_OFF);
  if (!armed) return;  // let go too early, or thought better of it

  printf("main: factory reset gesture confirmed; erasing\n");
  storage_erase();
  settings_reset();
  // Templates too. A device handed on with the previous owner's finger still
  // enrolled would authorise them on the new owner's account — which is most of
  // the reason to reset at all.
  fingerprint_erase_all();
  sleep_ms(50);
  // The identity is regenerated on the way back up, so the device comes back
  // as though it had never been used.
  watchdog_reboot(0, 0, 0);
}

// How often to ask the sensor whether a finger is present.
//
// Each poll is a UART round trip, and with no finger the module answers almost
// at once, so this is cheap — but not free, and there is no reason to ask
// hundreds of times a second.
#define FINGERPRINT_POLL_MS 200

// Mirrors the indicator state onto the module's own LED ring.
//
// Only on change: every call is a UART exchange, and repainting a steady light
// sixty times a second would flood the link the sensor also answers on.
static void mirror_light(status_led_mode_t mode) {
  static status_led_mode_t shown = (status_led_mode_t)-1;
  if (!fingerprint_present() || mode == shown) return;
  shown = mode;

  switch (mode) {
    case STATUS_LED_BREATHE: fingerprint_light(FP_LIGHT_BREATHE, FP_LED_BLUE, 0); break;
    case STATUS_LED_CONFIRM: fingerprint_light(FP_LIGHT_FLASH, FP_LED_GREEN, 3); break;
    case STATUS_LED_ARMED:   fingerprint_light(FP_LIGHT_STEADY, FP_LED_RED, 0); break;
    default:                 fingerprint_light(FP_LIGHT_OFF, FP_LED_OFF, 0); break;
  }
}

// A recognised fingerprint authorises exactly what a button press does.
//
// The sensor replaces the button as the presence gesture without changing what
// presence *means*, so everything downstream — the signing window, the typed
// PIN, the acknowledgement — is untouched.
static void poll_fingerprint(void) {
  if (!fingerprint_present()) return;

  static uint32_t last_poll;
  if ((now_ms() - last_poll) < FINGERPRINT_POLL_MS) return;
  last_poll = now_ms();

  uint16_t slot = 0, score = 0;
  if (!fingerprint_verify(&slot, &score)) return;

  printf("main: fingerprint matched slot %u (score %u)\n", slot, score);
  config_console_send_line("EVENT FINGERPRINT");
  handle_press();
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

  // Card power-on, not the first APDU. A Mac with no driver for this card
  // powers it and powers it off again without asking anything, so an
  // APDU-based trigger leaves the device stranded in pinpad mode precisely
  // where it most needs to recover. Measured: IccPowerOn, then IccPowerOff
  // 7.5 s later, and not one APDU in between.
  uint32_t contacted = usb_ccid_powered_ms();
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

// Upgrades into pinpad mode once our driver announces itself.
//
// In standard mode the card answers the private AID as a probe. Only our driver
// knows to ask for it, so a select is proof it is installed — and the device
// switches itself rather than waiting for an installer to say so.
//
// The delay lets the response to that select reach the host before the card
// disappears. It is kept short deliberately: until the reboot the card is
// answering both AIDs, which leaves two drivers in the running, and that state
// is exactly what must not persist.
#define UPGRADE_SETTLE_MS 300

static void upgrade_if_driver_present(void) {
  if (settings_aid_mode() == AID_MODE_PINPAD) return;

  uint32_t requested = piv_upgrade_requested_ms();
  if (requested == 0) return;
  if ((now_ms() - requested) < UPGRADE_SETTLE_MS) return;

  printf("main: driver selected the private AID; upgrading to pinpad\n");
  config_console_send_line("EVENT AID_UPGRADE pinpad");
  if (settings_set_aid_mode(AID_MODE_PINPAD)) {
    sleep_ms(50);
    watchdog_reboot(0, 0, 0);
  }
}

int main(void) {
  stdio_init_all();
  printf("\nsmart-card (rp2040) starting\n");

  button_init();
  settings_init();
  fingerprint_init();

  storage_init();
  status_led_init();
  factory_reset_gesture();
  piv_init();

  // A device that has never been given an identity makes its own, so a unit can
  // be flashed and boxed without any key material ever existing outside it.
  //
  // Only when there is nothing at all: a provisioned identity in flash, or one
  // compiled into secrets.h, is left alone. Otherwise every developer build
  // would silently replace the identity their Mac is already paired with.
  if (!piv_has_identity()) {
    if (identity_generate()) piv_reload_keys();
  }
  config_console_init();
  usb_ccid_start(piv_handle_apdu);

  // No RTOS. Everything runs here: USB, the console, and the button. The only
  // rule is that nothing may block long enough to starve tud_task(), which is
  // why the console pumps USB itself while waiting for a press.
  while (true) {
    tud_task();
    config_console_poll();
    upgrade_if_driver_present();
    revert_if_unclaimed();

    status_led_mode_t indicator = led_mode();
    status_led_update(indicator);
    mirror_light(indicator);
    poll_fingerprint();

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
        confirm_until_ms = now_ms() + CONFIRM_MS;
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
