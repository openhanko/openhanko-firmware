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
  if (!otp_access(OTP_SECRET_ROW, out, OTP_SECRET_LEN, false)) return false;

  // An unwritten page reads back as zeroes, so all-zero means blank rather than
  // a secret that happens to be zero. The odds of a real one colliding are the
  // odds of drawing 32 zero bytes from the TRNG, and provisioning rejects that
  // draw anyway.
  for (uint32_t i = 0; i < OTP_SECRET_LEN; i++) {
    if (out[i] != 0) return true;
  }
  return false;
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
  bool all_zero = true;
  for (uint32_t i = 0; i < OTP_SECRET_LEN; i += sizeof(uint64_t)) {
    uint64_t r = get_rand_64();
    memcpy(secret + i, &r, sizeof(r));
  }
  for (uint32_t i = 0; i < OTP_SECRET_LEN; i++) {
    if (secret[i] != 0) { all_zero = false; break; }
  }
  // Not superstition: all-zero is the blank pattern, so a secret of zeroes would
  // read back as "never provisioned" and the page would be burned for nothing.
  if (all_zero) {
    printf("otp: refusing to write an all-zero secret\n");
    return false;
  }

  bool ok = otp_access(OTP_SECRET_ROW, secret, OTP_SECRET_LEN, true);

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
