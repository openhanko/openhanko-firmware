#include "identity.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha1.h"
#include "mbedtls/x509_crt.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "storage.h"

// Certificates outlive any plausible service life of the hardware. There is no
// clock on this device and nothing checks expiry — macOS pairs against the
// public key, not the validity window — so a long fixed span avoids a device
// that mysteriously stops working on a date nobody chose deliberately.
#define CERT_NOT_BEFORE "20200101000000"
#define CERT_NOT_AFTER  "20450101000000"

// PEM output. Static rather than stack: a P-256 certificate is well under a
// kilobyte, but x509write already wants several kilobytes of stack of its own
// and the main loop's stack is not the place to find out it was short.
static char pem_buffer[2048];
static char subject[96];
static char common_name[64];

static int identity_rng(void *ctx, unsigned char *out, size_t len) {
  (void)ctx;
  while (len >= sizeof(uint64_t)) {
    uint64_t value = get_rand_64();
    memcpy(out, &value, sizeof(value));
    out += sizeof(value);
    len -= sizeof(value);
  }
  if (len) {
    uint64_t value = get_rand_64();
    memcpy(out, &value, len);
  }
  return 0;
}

// Names a key by the tail of its public-key fingerprint.
//
// Not the certificate's fingerprint, which cannot work: the name goes inside
// the certificate, so hashing the certificate to name itself is circular. The
// public key is settled the moment it is generated.
//
// SHA-1 of the raw public key is also how macOS derives
// kSecAttrApplicationLabel, which is the hash `sc_auth` pairs against. So this
// label matches the tail of the corresponding entry in `sc_auth list`, and two
// devices on one Mac can be told apart without unplugging either.
static bool key_label(const mbedtls_pk_context *key, char *out, size_t cap) {
  unsigned char buffer[160];
  unsigned char *end = buffer + sizeof(buffer);
  unsigned char *start = end;

  // Writes backwards from the end of the buffer and returns the length, so the
  // key material begins at `start` after the call moves it.
  int written = mbedtls_pk_write_pubkey(&start, buffer, key);
  if (written < 0) return false;

  unsigned char digest[20];
  if (mbedtls_sha1(start, (size_t)written, digest) != 0) return false;

  snprintf(out, cap, "%s #%02X%02X%02X", DEVICE_NAME,
           digest[17], digest[18], digest[19]);
  return true;
}

const char *identity_common_name(void) {
  return common_name[0] ? common_name : DEVICE_NAME;
}

bool identity_present(void) {
  return storage_loaded();
}

// Generates one key and its self-signed certificate, staging both.
static bool generate_slot(storage_slot_t cert_slot, storage_slot_t key_slot,
                          const char *label) {
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  bool ok = false;

  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);

  if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) goto done;
  if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                          identity_rng, NULL) != 0) {
    goto done;
  }

  // Private key first: if the certificate fails afterwards the whole staging
  // is discarded anyway, and this way the expensive step is done once.
  memset(pem_buffer, 0, sizeof(pem_buffer));
  if (mbedtls_pk_write_key_pem(&key, (unsigned char *)pem_buffer, sizeof(pem_buffer)) != 0) {
    goto done;
  }
  if (!storage_stage_append(key_slot, pem_buffer, strlen(pem_buffer))) goto done;

  char name[sizeof(common_name)];
  if (!key_label(&key, name, sizeof(name))) goto done;
  // Slot 9A is the identity macOS shows and pairs against, so its label is the
  // one the device reports as its own.
  if (cert_slot == STORAGE_CERT_9A) {
    snprintf(common_name, sizeof(common_name), "%s", name);
  }
  snprintf(subject, sizeof(subject), "CN=%s %s", name, label);

  // Self-signed: subject and issuer are the same key. Nothing verifies a chain
  // here — macOS pairs against the public key itself, so a CA would be
  // ceremony without benefit.
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0) goto done;
  if (mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0) goto done;
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);

  unsigned char serial[16];
  identity_rng(NULL, serial, sizeof(serial));
  serial[0] &= 0x7f;  // keep the DER INTEGER positive
  if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0) goto done;

  if (mbedtls_x509write_crt_set_validity(&crt, CERT_NOT_BEFORE, CERT_NOT_AFTER) != 0) goto done;
  if (mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0) goto done;
  if (mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0) {
    goto done;
  }

  memset(pem_buffer, 0, sizeof(pem_buffer));
  if (mbedtls_x509write_crt_pem(&crt, (unsigned char *)pem_buffer, sizeof(pem_buffer),
                                identity_rng, NULL) != 0) {
    goto done;
  }
  if (!storage_stage_append(cert_slot, pem_buffer, strlen(pem_buffer))) goto done;

  ok = true;

done:
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  memset(pem_buffer, 0, sizeof(pem_buffer));
  return ok;
}

bool identity_generate(void) {
  printf("identity: generating a fresh P-256 identity, this takes a moment\n");

  storage_stage_reset();
  if (!generate_slot(STORAGE_CERT_9A, STORAGE_KEY_9A, "authentication")) {
    printf("identity: slot 9A generation failed\n");
    storage_stage_reset();
    return false;
  }
  if (!generate_slot(STORAGE_CERT_9D, STORAGE_KEY_9D, "key management")) {
    printf("identity: slot 9D generation failed\n");
    storage_stage_reset();
    return false;
  }
  if (!storage_stage_commit()) {
    printf("identity: commit failed\n");
    return false;
  }

  printf("identity: generated \"%s\"\n", identity_common_name());
  return true;
}
