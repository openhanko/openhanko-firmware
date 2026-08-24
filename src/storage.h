#pragma once

#include <stdbool.h>
#include <stddef.h>

// Persistent PIV identity.
//
// One dedicated flash sector holds the four PEM strings. There is no wear
// levelling and no journal: this is written once during provisioning and read
// at every boot, so a log-structured store would be complexity for nothing.
//
// The record is encrypted with AES-256-GCM under a key derived from the
// device's OTP secret, so a flash reader recovers ciphertext and a tag rather
// than four PEMs. See otp.h for why that is worth nothing without secure boot
// and a fused debug port, and worth a great deal with them.

typedef enum {
  STORAGE_CERT_9A = 0,
  STORAGE_KEY_9A,
  STORAGE_CERT_9D,
  STORAGE_KEY_9D,
  STORAGE_SLOT_COUNT,
} storage_slot_t;

// True when all four slots hold plausible PEM data.
bool storage_loaded(void);

// NUL-terminated PEM for a slot, or NULL when unprovisioned.
const char *storage_get(storage_slot_t slot);

void storage_init(void);

// Staging: written by the console's PROVISION_* commands, committed as one
// flash write so a interrupted provisioning cannot leave a half-written
// identity behind.
void storage_stage_reset(void);
bool storage_stage_append(storage_slot_t slot, const void *data, size_t length);
bool storage_stage_commit(void);

// Erases the sector, returning the device to the compiled-in identity if any.
bool storage_erase(void);

storage_slot_t storage_slot_by_name(const char *name);
