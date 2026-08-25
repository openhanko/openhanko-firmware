#include <stdio.h>

#include "board_config.h"
#include "button.h"
#include "config_console.h"
#include "fingerprint.h"
#include "identity.h"
#include "hardware/watchdog.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "otp.h"
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

// Six random digits for the PIN prompt.
//
// The card accepts any PIN, so this value authenticates nothing and is not a
// secret — the presence check is the whole gate. It is random rather than fixed
// so that nobody learns a number and comes to believe it matters: someone who
// knows "the PIN is 000000" knows something untrue, and typing it by hand
// achieves nothing, because the signature that follows still waits on a finger.
//
// One draw covers six digits; get_rand_64() is ring-oscillator jitter on an
// ring-oscillator jitter, which is far better than needed for a value that is
// not a secret.
static void random_pin(char *out, size_t digits) {
  uint64_t r = get_rand_64();
  for (size_t i = 0; i < digits; i++) {
    out[i] = (char)('0' + (uint8_t)(r % 10));
    r /= 10;
  }
  out[digits] = '\0';
}

// The single funnel for "the user proved they are here". Its only caller is a
// fingerprint match — the button cannot reach it, and there is no build in which
// it can.
//
// last_press_ms is set here rather than at each call site because the pinpad
// branch reads it to decide whether presence was proved recently enough to
// answer a PIN request. Setting it only on the button path — which is what this
// did before — meant a fingerprint match authorised signing but never completed
// the pinpad exchange, leaving macOS waiting on a card that had already agreed.
//
// The event is EVENT TOUCH rather than the EVENT PRESS it was under the button
// build. Nothing parses it; the traces in the READMEs that still read
// EVENT BUTTON are the originals from that build, kept as taken.
static void note_presence(const char *source) {
  piv_set_presence_source(source);
  last_press_ms = now_ms();
  confirm_until_ms = now_ms() + CONFIRM_MS;
  printf("main: presence from %s; authorizing PIV and typing the dummy PIN\n", source);
  config_console_send_line("EVENT TOUCH");
  piv_note_user_presence();
  // Type unless the driver is handling this particular request.
  //
  // Not "unless we are in pinpad mode", which is what this used to say and which
  // broke the lock screen: pinpad governs the CryptoTokenKit-to-card leg, and
  // the lock screen does not use it. It collects the PIN in a text field the
  // same way stock sudo does, so a device in pinpad mode typed nothing and the
  // field stayed empty — the sensor read the finger and the Mac stayed locked.
  //
  // An outstanding PC_to_RDR_Secure is the actual signal that the driver has
  // this one covered. When there is none, something else may be waiting on a
  // keystroke and there is no way to find out which: the card is told nothing
  // until a PIN has already been submitted, so typing on presence is the only
  // mechanism available. The cost is that an idle touch puts six digits into
  // whatever has focus.
  if (!usb_ccid_pin_pending()) {
    char pin[PIV_DUMMY_PIN_DIGITS + 1];
    random_pin(pin, PIV_DUMMY_PIN_DIGITS);
    if (!usb_hid_type_line(pin)) {
      printf("main: HID interface was not ready; PIN not typed\n");
      config_console_send_line("EVENT PIN_NOT_TYPED");
    }
  }
}

// Slowest the ring is repainted during the reset gesture. Each repaint is a
// UART exchange, so the blink cannot be driven at the 5 ms the loop runs at.
#define RING_MIN_INTERVAL_MS 60

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

  // On the production board there is no discrete LED, so this gesture would run
  // with no feedback at all — an irreversible operation, invisible. The
  // accelerating blink exists precisely to warn that one is approaching, so it
  // has to reach the module's ring.
  //
  // Rate-limited because each call is a UART exchange, and the half-period falls
  // to 50 ms near the end. Skipping a toggle simply leaves the ring in its last
  // state a little longer, which on a module reads as the blink saturating
  // rather than as a fault.
  uint32_t last_ring_ms = 0;
  bool ring_lit = false;

  while (button_is_down()) {
    uint32_t now = now_ms();
    uint32_t step = now - last;
    last = now;
    uint32_t held = now - started;

    if (held >= RESET_ARM_MS) {
      if (!armed) {
        // Solid, once, on the transition. "Armed, release to erase" has to be
        // unmistakably different from the blinking that preceded it.
        fingerprint_light(FP_LIGHT_STEADY, FP_LED_RED, 0);
        ring_lit = true;
      }
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
    if (lit != ring_lit && (now - last_ring_ms) >= RING_MIN_INTERVAL_MS) {
      ring_lit = lit;
      last_ring_ms = now;
      fingerprint_light(lit ? FP_LIGHT_STEADY : FP_LIGHT_OFF,
                        lit ? FP_LED_RED : FP_LED_OFF, 0);
    }
    sleep_ms(5);
  }

  status_led_update(STATUS_LED_OFF);
  fingerprint_light(FP_LIGHT_OFF, FP_LED_OFF, 0);
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

static bool enroll_owns_ring;

// Mirrors the indicator state onto the module's own LED ring.
//
// Only on change: every call is a UART exchange, and repainting a steady light
// sixty times a second would flood the link the sensor also answers on.
// Forces the next mirror_light() to repaint even if the mode has not changed.
// The module drives its own ring during a rejected match, so our idea of what is
// showing can be wrong without the indicator state having moved.
static bool mirror_light_stale;
static void mirror_light_invalidate(void) { mirror_light_stale = true; }

// Holds the ring for the power-up flash.
//
// Without it the flash would never be seen: mirror_light()'s `shown` starts
// unset, so its first call always writes, and an idle device writes
// STATUS_LED_OFF. The window ends on its own and the indicator takes over.
//
// It has to end close to when the flash does, and not a moment after. The module
// reverts to breathing its own blue the instant a bounded effect finishes — the
// same behaviour that made the mismatch warning below use an infinite cycle
// count — so any slack here is not dark, it is the module's idle colour showing
// through — breathing blue, which is the one thing the ring does entirely on its
// own. At 1500 ms that was a second of it after two yellow flashes, reading as a
// state the device had entered rather than as an overrun. Two flashes take about
// 500 ms; the margin is for the module's cadence, and erring short costs a
// truncated flash instead of a colour that means something else.
#define BOOT_LIGHT_MS 700
static uint32_t boot_light_until;

static void mirror_light(status_led_mode_t mode) {
  static status_led_mode_t shown = (status_led_mode_t)-1;
  if (mirror_light_stale) { mirror_light_stale = false; shown = (status_led_mode_t)-1; }
  if (enroll_owns_ring) {
    // Force a repaint when enrollment gives the ring back, or the indicator
    // would sit in whatever colour enrollment left behind.
    shown = (status_led_mode_t)-1;
    return;
  }
  if (boot_light_until) {
    if ((int32_t)(now_ms() - boot_light_until) < 0) {
      shown = (status_led_mode_t)-1;
      return;
    }
    boot_light_until = 0;
  }
  if (!fingerprint_present() || mode == shown) return;
  shown = mode;

  switch (mode) {
    case STATUS_LED_BREATHE: fingerprint_light(FP_LIGHT_BREATHE, FP_LED_BLUE, 0); break;
    case STATUS_LED_CONFIRM: fingerprint_light(FP_LIGHT_FLASH, FP_LED_GREEN, 3); break;
    case STATUS_LED_ARMED:   fingerprint_light(FP_LIGHT_STEADY, FP_LED_RED, 0); break;
#if FINGERPRINT_IDLE_GLOW
    // White, and separated from the waiting state by colour rather than by
    // intensity. It is all three channels and so the brightest thing the ring
    // does, which is the opposite of dim — but breathing still spends half its
    // period near off, and an unlit device on a dark desk cannot be found at all.
    //
    // Driven rather than left alone. An unbounded breathe sticks, where a
    // bounded effect would end and let the module fall back to the blue
    // breathing it does by default — the same behaviour that filled the gaps in
    // the mismatch warning and outlasted the power-up flash. Better that the
    // idle state is ours than the module's.
    default:                 fingerprint_light(FP_LIGHT_BREATHE, FP_LED_WHITE, 0); break;
#else
    default:                 fingerprint_light(FP_LIGHT_OFF, FP_LED_OFF, 0); break;
#endif
  }
}

// ---------------------------------------------------------------- enrollment
//
// The button never authenticates, so it is free to
// mean something else, and what it means is "I want to add a finger". The click
// alone only marks intent: what authorises the operation is the finger resting
// on the sensor at the moment of the click, which must already be enrolled.
//
// That matters because enrollment is the one operation that keeps the key and
// adds a way to use it. Someone who has stolen the device can wipe it — that
// destroys the key, so they gain nothing — but they must not be able to add
// their own finger to a working device and walk away with a credential that
// answers to them.

// How many fingers a device insists on before it considers itself set up.
//
// One. The argument for two was that there is no fallback if the enrolled finger
// becomes unavailable — but factory reset is a fallback: hold the button through
// power-up, enrol again, pair again, a minute in total. For unlocking a Mac that
// is a complete recovery, and making every user enrol twice to avoid it costs
// more than it saves.
//
// It is not free. A reset destroys the key, so anything bound to it that is
// expensive to re-provision — an SSH identity trusted by many hosts, a
// CA-issued certificate — has to be redone. Someone using the device that way
// should enrol a second finger through the gate, which is what it is for.
#define ENROLL_MINIMUM 1

// Impressions per finger. Two is the floor for a template that matches
// reliably; the module's self-learning improves it with use. One captures a
// single finger position and shows up later as intermittent non-recognition.
#define ENROLL_IMPRESSIONS 2

#define ENROLL_ARM_MS     30000   // to present the new finger
#define ENROLL_CAPTURE_MS 20000   // to complete the impressions once started
#define ENROLL_SETTLE_MS  1200    // how long the result stays lit

typedef enum {
  ENROLL_IDLE = 0,
  ENROLL_WAIT_LIFT,   // the gate finger is probably still down; wait for it off
  ENROLL_WAIT_TOUCH,  // ring breathing, waiting for the finger to enrol
  ENROLL_SETTLE,      // showing the outcome before releasing the ring
} enroll_state_t;

static enroll_state_t enroll_state;
static uint32_t enroll_deadline;
static void enroll_open(const char *why) {
  enroll_state = ENROLL_WAIT_LIFT;
  enroll_deadline = now_ms() + ENROLL_ARM_MS;
  enroll_owns_ring = true;
  printf("main: enrollment open (%s)\n", why);
  config_console_send_line("EVENT ENROLL_OPEN");
  fingerprint_light(FP_LIGHT_BREATHE, FP_LED_PURPLE, 0);
}

static void enroll_finish(bool ok) {
  config_console_send_line(ok ? "EVENT ENROLL_OK" : "EVENT ENROLL_FAILED");
  // The module has been driving the ring during the enrolment itself, so this
  // is where the indicator comes back to us — steady, briefly, and then either
  // re-armed or dark.
  fingerprint_light(FP_LIGHT_STEADY, ok ? FP_LED_GREEN : FP_LED_RED, 0);
  enroll_state = ENROLL_SETTLE;
  enroll_deadline = now_ms() + ENROLL_SETTLE_MS;
}

// Opens enrollment if the finger on the sensor says it may.
static void enroll_gate(void) {
  if (enroll_state != ENROLL_IDLE || !fingerprint_present()) return;

  // A click with nothing on the sensor is not a refusal, it is not a request at
  // all — so say nothing. Flashing red there answers a question nobody asked,
  // and worse, it makes the one gesture that is supposed to do nothing look like
  // it did something and failed.
  if (fingerprint_touch_wired() && !fingerprint_finger_down()) return;

  if (fingerprint_template_count() == 0) {
    // Nothing to match against, and nothing yet to protect: with no templates
    // the device cannot authenticate for anybody, so there is no capability
    // here to escalate. This is the same door the boot-time arm walks through.
    enroll_open("no finger enrolled yet");
    return;
  }

  uint16_t slot = 0, score = 0;
  if (fingerprint_verify(&slot, &score)) {
    // Deliberately not note_presence(): the user pressed the button to
    // configure the device, and a gesture that silently authorised whatever
    // macOS happened to be waiting for would be a surprise.
    printf("main: enrollment gate opened by slot %u (score %u)\n", slot, score);
    // Two flashes for yes and one long for no. Green against red is the pair
    // red-green colourblindness collapses, and at the gate this flash is the
    // only feedback there is, so the count carries the meaning and the colour
    // only reinforces it.
    fingerprint_light(FP_LIGHT_FLASH, FP_LED_GREEN, 2);
    enroll_open("gated by a matching finger");
  } else {
    printf("main: enrollment refused: no matching finger on the sensor\n");
    config_console_send_line("EVENT ENROLL_REFUSED");
    fingerprint_light(FP_LIGHT_FLASH, FP_LED_RED, 1);
    // The module lights its own ring during the failed match and leaves it
    // breathing. Hand the indicator back, or it stays lit until something else
    // happens to change the mode.
    mirror_light_invalidate();
  }
}

static void poll_enrollment(void) {
  if (enroll_state == ENROLL_IDLE) return;

  if (enroll_state == ENROLL_SETTLE) {
    if ((int32_t)(now_ms() - enroll_deadline) < 0) return;
    enroll_state = ENROLL_IDLE;
    enroll_owns_ring = false;
    // The module has had the ring throughout; take it back explicitly rather
    // than assuming our cached idea of what is showing still holds.
    mirror_light_invalidate();
    // A device with no finger enrolled is inert, so keep asking rather than
    // going dark and leaving the user with nothing to act on.
    if (fingerprint_present() && fingerprint_template_count() < ENROLL_MINIMUM) {
      enroll_open("fewer fingers enrolled than the minimum");
    }
    return;
  }

  if ((int32_t)(now_ms() - enroll_deadline) >= 0) {
    printf("main: enrollment window expired\n");
    config_console_send_line("EVENT ENROLL_TIMEOUT");
    enroll_finish(false);
    return;
  }

  static uint32_t last_poll;
  if ((now_ms() - last_poll) < FINGERPRINT_POLL_MS) return;
  last_poll = now_ms();

  bool down = fingerprint_finger_down();

  if (enroll_state == ENROLL_WAIT_LIFT) {
    // The finger that opened the gate is still on the sensor, and enrolling it
    // would store a second copy of a template that already exists. Wait for it
    // to come off, so what gets enrolled is deliberately a different finger.
    if (!down) enroll_state = ENROLL_WAIT_TOUCH;
    return;
  }

  if (!down) return;

  uint16_t slot = fingerprint_template_count();
  printf("main: enrolling into slot %u\n", (unsigned)slot);
  config_console_send_line("EVENT ENROLL_CAPTURING");
  // Blocks until the impressions are done. It pumps USB throughout, so the card
  // keeps answering, but nothing else in this loop runs — no console, no
  // button. Acceptable only because we call it with a finger already on the
  // sensor, so the wait is the enrollment itself rather than the user finding
  // the device. Worth making properly incremental once there is hardware to
  // test the state machine against.
  enroll_finish(fingerprint_auto_enroll(slot, ENROLL_IMPRESSIONS, ENROLL_CAPTURE_MS));
}

// A recognised fingerprint authorises exactly what a button press does.
//
// The sensor replaces the button as the presence gesture without changing what
// presence *means*, so everything downstream — the signing window, the typed
// PIN, the acknowledgement — is untouched.
// True once a match has been accepted, until the finger comes off again.
static bool finger_seen;

// Set at boot when the fingerprint module is not the one this device was bound
// to. Latched: nothing clears it but a factory reset.
static bool module_mismatch;


static void poll_fingerprint(void) {
  if (!fingerprint_present()) return;

  // With TouchOut wired this is a GPIO read, so the expensive part — a capture
  // and a search over UART — only happens when a finger is actually present.
  // Without it, fingerprint_finger_down() is itself a capture attempt and this
  // is merely where the polling happens.
  if (fingerprint_touch_wired() && !fingerprint_finger_down()) {
    finger_seen = false;
    return;
  }

  // One match per touch. A finger left resting on the sensor otherwise matches
  // again every poll — four EVENT FINGER for a single sudo — and each of those
  // is a full capture and search over the UART, so the link never goes quiet and
  // the presence window keeps being reopened by one continuous act of presence.
  // The button has had a minimum interval for exactly this reason; this is the
  // sensor's version of it, and with TouchOut wired the lift is a GPIO read.
  if (finger_seen) return;

  static uint32_t last_poll;
  if ((now_ms() - last_poll) < FINGERPRINT_POLL_MS) return;
  last_poll = now_ms();

  uint16_t slot = 0, score = 0;
  if (!fingerprint_verify(&slot, &score)) {
    // The module lights its own ring on a rejected match and leaves it breathing
    // afterwards. mirror_light() only writes on a change, so with the indicator
    // already OFF it saw nothing to do and the ring stayed lit. Invalidate the
    // cache so the next pass repaints whatever the indicator actually wants.
    mirror_light_invalidate();
    // Also wait for a lift after a rejection, or the same unrecognised finger is
    // re-tested several times a second for as long as it rests there.
    finger_seen = true;
    return;
  }

  finger_seen = true;
  printf("main: fingerprint matched slot %u (score %u)\n", slot, score);
  config_console_send_line("EVENT FINGERPRINT");
  note_presence("FINGER");
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
  printf("\nopenhanko starting\n");

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
  // Only when there is nothing at all. An identity already in flash is left
  // alone, or every developer build would silently replace the one their Mac is
  // already paired with. Replacing it deliberately is the factory reset, which
  // is the only path there is now that GENERATE_IDENTITY is gone.
  if (!piv_has_identity()) {
    // The identity is wrapped with the device secret, so there has to be one
    // before there can be a key to store. Provisioning it here is the one moment
    // where burning an OTP page is unambiguously right: this device has no
    // identity, is about to acquire the only one it will ever have without a
    // factory reset, and cannot keep it otherwise.
    //
    // Only ever once. A device that already holds a secret keeps it — including
    // through a factory reset, which destroys the identity in flash and cannot
    // touch OTP — so the next identity is wrapped with the same secret.
    if (!otp_secret_present()) {
      if (otp_secret_provision()) {
        printf("main: burned a device secret into OTP\n");
        config_console_send_line("EVENT OTP_PROVISIONED");
      } else {
        printf("main: no device secret and could not make one; identity not stored\n");
        config_console_send_line("EVENT OTP_FAILED");
      }
    }
    if (identity_generate()) piv_reload_keys();
  }
  config_console_init();
  usb_ccid_start(piv_handle_apdu);

  // Bind to the fingerprint module, or notice that it has changed.
  //
  // Recorded on a device that has no module stored, which after a factory reset
  // is whatever is in front of it. A device that meets a different module later
  // refuses everything until it is reset — see settings.h for why that is the
  // only recovery offered.
  if (fingerprint_present()) {
    uint8_t serial[SETTINGS_SERIAL_LEN];
    if (fingerprint_chip_serial(serial)) {
      if (!settings_module_bound()) {
        if (settings_bind_module(serial)) {
          printf("main: bound to this fingerprint module\n");
          config_console_send_line("EVENT MODULE_BOUND");
        }
      } else if (memcmp(serial, settings_module_serial(), sizeof(serial)) != 0) {
        module_mismatch = true;
        printf("main: FINGERPRINT MODULE CHANGED; refusing everything\n");
        config_console_send_line("EVENT MODULE_MISMATCH");
      }
    } else {
      // A module that answers the handshake but not PS_GetChipSN is not one we
      // can vouch for either. Fail closed.
      module_mismatch = settings_module_bound();
      if (module_mismatch) {
        printf("main: fingerprint module would not identify itself\n");
        config_console_send_line("EVENT MODULE_UNIDENTIFIED");
      }
    }
  }
  piv_set_module_mismatch(module_mismatch);

  // A device with an identity but no enrolled finger cannot authenticate for
  // anyone, because the button does not do it. So there is no useful state
  // between blank and enrolled, and nothing is gained by making the user
  // discover a gesture to leave it: offer enrollment immediately, and keep
  // offering until it takes.
  //
  // This also closes the window the gesture cannot cover. The gate needs a
  // matching finger, and a device with no templates has none — so first
  // enrollment can never be authorised by a finger, and would otherwise have to
  // be authorised by possession alone. Doing it at first boot means there is no
  // period during which the device is paired and useful but unenrolled, which
  // is the only period in which appropriating it would be worth anything.
  if (fingerprint_present() && fingerprint_template_count() < ENROLL_MINIMUM) {
    enroll_open("fewer fingers enrolled than the minimum");
  } else if (fingerprint_present() && !module_mismatch) {
    // Say something on power-up.
    //
    // A provisioned unit used to blink on its way through the SDK's double-reset
    // bootloader window. That is gone — the bootrom's own double-tap replaced it
    // and runs silently — so the device now boots, binds, and sits dark, which
    // looks exactly like one that is dead.
    //
    // Yellow because the ring cannot do orange. PS_ControlBLN takes a three-bit
    // mask, one bit per channel, so the entire palette is the eight combinations
    // of red, green and blue; orange needs green at partial intensity and there
    // is no intensity to set. Of what exists, yellow is nearest, and it is not
    // one of the four the device already uses to mean something.
    fingerprint_light(FP_LIGHT_FLASH, FP_LED_YELLOW, 2);
    boot_light_until = now_ms() + BOOT_LIGHT_MS;
  }

  // No RTOS. Everything runs here: USB, the console, and the button. The only
  // rule is that nothing may block long enough to starve tud_task(), which is
  // why the console pumps USB itself while waiting for a press.
  while (true) {
    tud_task();
    config_console_poll();
    upgrade_if_driver_present();
    revert_if_unclaimed();

    if (module_mismatch) {
      // Nothing else happens on a device whose sensor has been changed: no
      // matching, no enrolment, no configuration. The ring says so in the one
      // colour that means stop, and keeps saying it.
      // Cycle count 0 is an infinite loop, per the manual, so this is sent once
      // and runs until something stops it. Sending a bounded burst on a timer
      // left gaps, and the module filled them with the blue breathing it does by
      // default — a device refusing to work should not spend half its time
      // showing its idle colour.
      static bool warned;
      if (!warned) {
        warned = true;
        fingerprint_light(FP_LIGHT_FLASH, FP_LED_RED, 0);
      }
      tud_task();
      config_console_poll();
      continue;
    }

    // Keep the applet's idea of readiness current: a sensor with nothing
    // enrolled cannot authorise anything, and the status word should say so.
    piv_set_setup_incomplete(fingerprint_present() &&
                             fingerprint_template_count() == 0);

    status_led_mode_t indicator = led_mode();
    status_led_update(indicator);
    mirror_light(indicator);
    // Both want the sensor, and an enrollment in progress must not have its
    // impressions stolen by the authentication poll.
    if (enroll_state == ENROLL_IDLE) poll_fingerprint();
    poll_enrollment();

    if (usb_ccid_pin_pending()) {
      // The host is waiting on a pinpad PIN entry. Either the user presses now,
      // or they already pressed a moment ago to satisfy the PIN prompt that
      // triggered this request — both are the same physical act of presence, so
      // one press is enough.
      bool recent = last_press_ms != 0 &&
                    (now_ms() - last_press_ms) < PRESS_ANSWERS_PINPAD_MS;
      if (recent) {
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
      // The button has exactly two jobs, and neither is authentication: the
      // factory reset gesture, which is handled at boot, and opening
      // enrollment. What authorises the enrollment is the finger on the sensor,
      // not the click.
      enroll_gate();
    }
  }
}
