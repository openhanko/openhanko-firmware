#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void piv_init(void);
void piv_reload_keys(void);

// True once a usable certificate is loaded, from either source.
bool piv_has_identity(void);

// Where the loaded identity came from: "flash" or "none". There is one source:
// the key the device generated for itself at first boot.
const char *piv_key_source_name(void);

// "rsa2048", "p256", or "none".
const char *piv_algorithm_name(void);

// 0, or the mbedTLS error from the last private-key parse.
int piv_key_parse_error(void);

// Opens a short window during which the authentication key (slot 9a) may be
// used once. Called after a fingerprint match.
void piv_note_user_presence(void);

// Labels the next trace entry — "FINGER" is the only source that authorises
// anything. Purely diagnostic.
void piv_set_presence_source(const char *source);

// Tells the applet that nothing can be authorised yet because a sensor is
// fitted and no finger is enrolled. GENERAL AUTHENTICATE then answers 6985
// rather than 6982, so a host can tell "touch it" from "set it up first".
void piv_set_setup_incomplete(bool incomplete);

// Tells the applet the sensor has been swapped. Everything is refused with 6983
// until a factory reset, which is the only recovery.
void piv_set_module_mismatch(bool mismatch);
bool piv_module_mismatch(void);

// Marks the PIN as verified without an APDU. Used by the pinpad path, where
// the host delegates PIN entry to the reader and no VERIFY ever arrives.
void piv_note_pin_verified(void);

// True while macOS is working through an authentication and no press has
// answered it yet. Drives the breathing status LED.
bool piv_challenge_active(void);

// True for a moment after a successful signature, to confirm it visually.
bool piv_recent_signature(void);

// True once our driver has selected the private AID, which nothing else on the
// system knows to ask for — so it is proof the driver is installed and bound.
bool piv_private_aid_selected(void);

// The parsed slot 9A key, or NULL when the device holds no identity. Exposed so
// the label can be derived from whatever key is actually loaded, rather than
// only from one the device generated this boot.
const void *piv_auth_key(void);

// Milliseconds since boot when the host first sent any APDU, or 0 if it never
// has. A device on a charger, or in a hub with no host, stays at 0.
uint32_t piv_first_contact_ms(void);

// When the private AID was selected while in standard mode, or 0 if it has not
// been. That select can only have come from our driver, so it is the signal to
// upgrade into pinpad mode.
uint32_t piv_upgrade_requested_ms(void);

bool piv_handle_apdu(const uint8_t *apdu, size_t apdu_len,
                     uint8_t *response, size_t *response_len,
                     size_t response_cap);
