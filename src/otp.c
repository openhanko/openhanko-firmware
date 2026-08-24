#include "otp.h"

#include <stdio.h>
#include <string.h>

#include "pico.h"

#include "boot/bootrom_constants.h"
#include "mbedtls/sha256.h"
#include "pico/bootrom.h"
#include "pico/rand.h"

// With the ECC flag set, the bootrom presents each OTP row as two bytes and
// handles the Hamming code itself. Without it a row is three raw bytes and the
// correction is the caller's problem, which is not a problem worth owning.
#define OTP_ROW_BYTES 2u

static bool otp_access(uint16_t row, uint8_t *buf, uint32_t len, bool write) {
  otp_cmd_t cmd = {
      .flags = (uint32_t)row | OTP_CMD_ECC_BITS | (write ? OTP_CMD_WRITE_BITS : 0u),
  };
  int rc = rom_func_otp_access(buf, len, cmd);
  if (rc != 0) {
    printf("otp: %s row 0x%03x failed, rc=%d\n", write ? "write" : "read", row, rc);
    return false;
  }
  return true;
}

// Expands each bit into itself followed by its complement, low bit first.
//
// The pairing is what the chaffing is for, so the ordering has to be identical
// on both sides or the readback is silently wrong — hence one place that lays
// them out and one that takes them apart, rather than the same arithmetic twice.
static void chaff(const uint8_t *plain, uint32_t len, uint8_t *out) {
  memset(out, 0, len * 2);
  for (uint32_t i = 0; i < len * 8u; i++) {
    bool bit = (plain[i / 8u] >> (i % 8u)) & 1u;
    uint32_t lo = i * 2u, hi = lo + 1u;
    if (bit)  out[lo / 8u] |= (uint8_t)(1u << (lo % 8u));
    if (!bit) out[hi / 8u] |= (uint8_t)(1u << (hi % 8u));
  }
}

// Reverses it, and checks every pair really is complementary.
//
// A pair that is not is either a failed write or a cell that has been meddled
// with, and both must read as "no usable secret" rather than as a plausible
// wrong answer — key material derived from a half-right secret would decrypt
// nothing and look like corrupted storage instead of a corrupted secret.
static bool unchaff(const uint8_t *stored, uint32_t len, uint8_t *out) {
  memset(out, 0, len);
  for (uint32_t i = 0; i < len * 8u; i++) {
    uint32_t lo = i * 2u, hi = lo + 1u;
    bool a = (stored[lo / 8u] >> (lo % 8u)) & 1u;
    bool b = (stored[hi / 8u] >> (hi % 8u)) & 1u;
    if (a == b) return false;
    if (a) out[i / 8u] |= (uint8_t)(1u << (i % 8u));
  }
  return true;
}

bool otp_selftest(uint32_t *chipid_lo) {
  if (!chipid_lo) return false;
  // Rows 0x000-0x001 are CHIPID0/CHIPID1, factory-programmed on every part, so a
  // zero here means the read failed rather than that the rows are empty.
  uint8_t buf[4] = {0};
  if (!otp_access(0x000, buf, sizeof(buf), false)) return false;
  *chipid_lo = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
               ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
  return *chipid_lo != 0;
}

bool otp_secret_read(uint8_t out[OTP_SECRET_LEN]) {
  if (!out) return false;
  memset(out, 0, OTP_SECRET_LEN);

  uint8_t stored[OTP_SECRET_STORED];
  if (!otp_access(OTP_SECRET_ROW, stored, sizeof(stored), false)) return false;

  // Blank reads as zeroes, and zeroes are not a valid chaffed value — every pair
  // would be 0,0 rather than complementary — so unchaff() rejects an unwritten
  // page without needing a separate emptiness test.
  bool ok = unchaff(stored, OTP_SECRET_LEN, out);
  memset(stored, 0, sizeof(stored));
  if (!ok) memset(out, 0, OTP_SECRET_LEN);
  return ok;
}

bool otp_secret_present(void) {
  uint8_t secret[OTP_SECRET_LEN];
  bool present = otp_secret_read(secret);
  memset(secret, 0, sizeof(secret));
  return present;
}

bool otp_secret_provision(void) {
  if (otp_secret_present()) {
    printf("otp: already provisioned; refusing to write again\n");
    return false;
  }

  uint8_t secret[OTP_SECRET_LEN];
  for (uint32_t i = 0; i < OTP_SECRET_LEN; i += sizeof(uint64_t)) {
    uint64_t r = get_rand_64();
    memcpy(secret + i, &r, sizeof(r));
  }

  uint8_t stored[OTP_SECRET_STORED];
  chaff(secret, OTP_SECRET_LEN, stored);
  bool ok = otp_access(OTP_SECRET_ROW, stored, sizeof(stored), true);
  memset(stored, 0, sizeof(stored));

  // Read back rather than trust the write. A page that took only partially is
  // the worst outcome available here — it cannot be rewritten, and a device that
  // believed it had a secret would wrap key material with something it could not
  // reproduce.
  if (ok) {
    uint8_t check[OTP_SECRET_LEN];
    ok = otp_secret_read(check) && memcmp(check, secret, OTP_SECRET_LEN) == 0;
    if (!ok) printf("otp: write did not verify; this page is now unusable\n");
    memset(check, 0, sizeof(check));
  }

  memset(secret, 0, sizeof(secret));
  return ok;
}

bool otp_secret_fingerprint(char *out, uint32_t cap) {
  if (!out || cap < 13) return false;
  uint8_t secret[OTP_SECRET_LEN];
  if (!otp_secret_read(secret)) {
    memset(secret, 0, sizeof(secret));
    return false;
  }

  // A hash, so the console can identify which secret a device holds without
  // carrying the secret itself. Six bytes is plenty to tell ten boards apart and
  // far too few to be worth attacking.
  uint8_t digest[32];
  mbedtls_sha256(secret, sizeof(secret), digest, 0);
  memset(secret, 0, sizeof(secret));

  for (int i = 0; i < 6; i++) {
    snprintf(out + (i * 2), cap - (uint32_t)(i * 2), "%02x", digest[i]);
  }
  return true;
}
