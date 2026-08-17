#pragma once

#include <stdbool.h>
#include <stdint.h>

// Device settings, in their own flash sector.
//
// Deliberately not part of the identity record in storage.c. That record is one
// CRC'd blob holding all four PEMs, so folding a setting into it would mean
// erasing and rewriting the private keys every time a mode changed — a lot of
// risk, and flash wear, for one byte.

// Which application the card answers, and therefore which driver macOS binds.
//
// Exactly one driver owns a card, and macOS decides that at insertion from the
// AID alone — there is no negotiation and no second chance. So the AID *is* the
// mode selector, and the two modes are mutually exclusive by construction.
typedef enum {
  // Answer the standard PIV AID. Apple's built-in pivtoken binds, so the device
  // works on any Mac with nothing installed: the button press types the PIN
  // over HID. This is the factory default and the safe state.
  AID_MODE_STANDARD = 0,

  // Answer only our private AID, making the card invisible to pivtoken so the
  // driver in macos/ binds deterministically. Buys the pinpad experience — a
  // press with no PIN typed — at the cost of being inert without that driver,
  // which is why the device reverts on its own when nothing claims it.
  AID_MODE_PINPAD = 1,
} aid_mode_t;

void settings_init(void);

aid_mode_t settings_aid_mode(void);

// Persists the mode. Returns false if the flash write did not verify; the
// caller should not reboot in that case, or the device comes back unchanged
// with no indication why.
bool settings_set_aid_mode(aid_mode_t mode);

const char *settings_aid_mode_name(aid_mode_t mode);

// Erases the settings sector, returning every setting to its compiled default.
// Part of a factory reset: a device being passed to someone else should behave
// exactly like one out of the box, which means the standard AID regardless of
// what the previous owner had installed.
bool settings_reset(void);
