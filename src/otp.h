#pragma once

#include <stdbool.h>
#include <stdint.h>

// A device secret held in the RP2350's one-time-programmable memory.
//
// The point of it is encryption at rest: key material in flash is wrapped with
// something the flash does not contain, so reading the flash yields ciphertext.
// On an RP2354A the flash is a second die in the same package, which already
// makes lifting it a laboratory job rather than a hot-air one — this is what
// makes the contents worthless once someone has managed it anyway.
//
// **It buys nothing on its own, and the dependency runs both ways.** With SWD
// open a debugger reads this out of OTP directly. With secure boot off, an
// attacker with BOOTSEL flashes firmware that reads it legitimately — it is
// secure-readable by design — and prints it. So this is the machinery that makes
// debug lockout and secure boot worth doing, not a defence that stands alone.
//
// Every bit here is permanent. There is no erase, no rewrite, and no way back:
// a row written wrong stays wrong for the life of the part. Nothing in this file
// is called automatically for that reason — provisioning happens when somebody
// asks for it and proves they are holding the device.

// The page map.
//
// Rows 0x0c0-0xf47 are undefined by the datasheet and ours to use. Locks are
// per *page* of 64 rows, and permanent, so the map has to be decided before the
// first lock rather than grown into: locking page 4 makes its other 48 rows
// unwritable for the life of the part, whatever we later wish we had put there.
//
// One purpose per page, therefore, and each locked only once its contents are
// final. A page is 128 bytes and the largest thing here is 32, which wastes
// three quarters of each — irrelevant against 3720 rows, and much cheaper than
// discovering a secret needs somewhere to live after the page holding its
// neighbours has been sealed.
//
//   page 4  0x100-0x13f  device secret, wraps key material at rest
//   page 5  0x140-0x17f  reserved: fingerprint link keys, if SecurLevel is ever
//                        enabled — PS_GetKeyt returns 32 bytes the MCU must keep,
//                        and it is generated at pairing, long after page 4 is
//                        sealed
//   page 6  0x180-0x1bf  reserved
//   page 7  0x1c0-0x1ff  reserved
//
// Pages 8 and up are untouched. Nothing is written to a reserved page and
// nothing locks one; they exist so that a later secret has somewhere to go that
// does not depend on a decision made today.
#define OTP_PAGE_ROWS 64u

// The secret is stored chaffed: every bit sits beside its own complement, so
// each adjacent pair reads as one-and-zero whichever way the bit went.
//
// That is aimed at one specific attack. IOActive's passive voltage contrast with
// a focused ion beam reads antifuse cells directly rather than through any access
// control, recovering the bitwise OR of adjacent bits — and it is the one finding
// from the Hacking Challenge that A4 does *not* fix. Against complementary pairs
// that OR is 1 everywhere and says nothing. It is Raspberry Pi's own published
// mitigation, and it costs only double the rows.
//
// Page 4 layout. Rows 0x100-0x11f are deliberately left blank: development
// boards wrote an unchaffed secret there before this existed, and OTP has no
// erase, so the region cannot be reused. Placing the real secret in the second
// half means one firmware works on those boards and on fresh ones alike, at a
// cost of 32 rows out of 3720.
#define OTP_SECRET_PAGE   4u
#define OTP_SECRET_ROW    0x120u
#define OTP_SECRET_LEN    32u
// Two bits stored per bit of secret.
#define OTP_SECRET_STORED (OTP_SECRET_LEN * 2u)

#define OTP_LINK_KEY_PAGE 5u
#define OTP_LINK_KEY_ROW  (OTP_LINK_KEY_PAGE * OTP_PAGE_ROWS) /* 0x140 */

// Reads a known-populated row and returns it, so a caller can tell a working
// OTP path from a broken one. A failed read leaves its buffer zeroed, which is
// also exactly what a blank page looks like — without this, "no secret" and "the
// bootrom refused us" are the same answer.
bool otp_selftest(uint32_t *chipid_lo);

// True when the page holds something other than the all-zero pattern an
// unwritten page reads as.
bool otp_secret_present(void);

// Reads the secret. False if the page is blank or the read is refused.
bool otp_secret_read(uint8_t out[OTP_SECRET_LEN]);

// Generates a secret and burns it in. **Irreversible.** Refuses if the page is
// already written, verifies by reading back, and returns false rather than
// leaving a half-written page unreported.
bool otp_secret_provision(void);

// A short identifier for the secret — the first bytes of its SHA-256 — so a
// device can be told apart and a provisioning step confirmed without the secret
// itself ever crossing the console.
bool otp_secret_fingerprint(char *out, uint32_t cap);
