#pragma once

#include <stdbool.h>

// Generates the device's PIV identity on the device itself.
//
// The alternative is generating keys on a workshop machine and pushing them
// over the console, which works but means the vendor holds a copy of every
// private key they ever shipped. Anyone with that key can build a card the
// user's Mac accepts as theirs, so it is not a small thing to hold — and the
// user has no way to verify what happened to it.
//
// Generating here removes the question rather than answering it: the private
// key never exists outside this chip, so there is nothing to store, back up,
// leak, or be asked about.
//
// The catch is the RNG, and it is a real one. This depends entirely on
// get_rand_64(), which on the RP2040 is seeded from ring-oscillator jitter and
// is not a trustworthy source for long-lived key material. Keys generated on an
// RP2040 are for development. On the RP2350 the same call is backed by the
// hardware TRNG, and only there is this genuinely sound.

// True if the device already holds an identity, generated or provisioned.
bool identity_present(void);

// Generates a fresh P-256 identity for slots 9A and 9D and commits it to flash,
// replacing whatever was there. Takes a couple of seconds and blocks; callers
// on the main loop should not run it while USB is expected to stay responsive.
bool identity_generate(void);

// The common name embedded in the generated certificates, which is also what
// macOS shows in its PIN prompt. Includes the board's unique ID so two devices
// are distinguishable on the same Mac.
const char *identity_common_name(void);
