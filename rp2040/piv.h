#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void piv_init(void);
void piv_reload_keys(void);

// True once a usable certificate is loaded, from either source.
bool piv_has_identity(void);

// Where the loaded identity came from: "nvs", "compiled", or "none".
// A console-provisioned identity takes precedence over one baked into the
// firmware by main/secrets.h.
const char *piv_key_source_name(void);

// "rsa2048", "p256", or "none".
const char *piv_algorithm_name(void);

// 0, or the mbedTLS error from the last private-key parse.
int piv_key_parse_error(void);

// Opens a short window during which the authentication key (slot 9a) may be
// used once. Called after a debounced button press.
void piv_note_user_presence(void);

// Marks the PIN as verified without an APDU. Used by the pinpad path, where
// the host delegates PIN entry to the reader and no VERIFY ever arrives.
void piv_note_pin_verified(void);

// True while macOS is working through an authentication and no press has
// answered it yet. Drives the breathing status LED.
bool piv_challenge_active(void);

// True for a moment after a successful signature, to confirm it visually.
bool piv_recent_signature(void);

// Pairing mode waives the per-signature presence requirement for a couple of
// minutes so `sc_auth pair` can complete its handshake unattended.
void piv_set_pairing_mode(bool enabled);
bool piv_pairing_mode_active(void);

// Times one RSA private-key operation, in milliseconds; 0 if no key is loaded.
// The RP2040 has no big-integer accelerator, so this is the number that decides
// whether the part is fast enough to be pleasant.
uint32_t piv_benchmark_sign(void);

bool piv_handle_apdu(const uint8_t *apdu, size_t apdu_len,
                     uint8_t *response, size_t *response_len,
                     size_t response_cap);
