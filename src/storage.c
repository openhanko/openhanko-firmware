#include "storage.h"

#include <string.h>

#include <stdio.h>

#include "hardware/flash.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "otp.h"
#include "pico/rand.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

// Bumped from SCK1 when the blob became ciphertext. An old record is rejected
// rather than migrated: migration code would have to read plaintext key
// material, which is the thing this change exists to make impossible, and it
// would be carried forever to serve devices that were never shipped.
#define STORAGE_MAGIC 0x53434B32u  // "SCK2"

#define STORAGE_NONCE_LEN 12
#define STORAGE_TAG_LEN   16
#define SLOT_CAP 2560

// Occupies the last three erase sectors, which is what the record needs once
// rounded up. Kept at the top of flash and outside the image, so a firmware
// update does not disturb a provisioned identity. PICO_FLASH_SIZE_BYTES comes
// from the board header, so
// this follows the part rather than assuming 2 MB.
#define STORAGE_REGION_SIZE (3 * FLASH_SECTOR_SIZE)
#define STORAGE_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - STORAGE_REGION_SIZE)

typedef struct {
  uint32_t magic;
  uint32_t length[STORAGE_SLOT_COUNT];
  // Fresh per write. Reusing a nonce under one key is how GCM fails
  // catastrophically rather than gracefully.
  uint8_t nonce[STORAGE_NONCE_LEN];
  uint8_t tag[STORAGE_TAG_LEN];
  uint32_t crc;
  // Ciphertext. The lengths stay in the clear — they say how big a key is, which
  // the certificate already tells anyone who looks — but the tag covers them, so
  // they cannot be edited without the decrypt failing.
  char blob[STORAGE_SLOT_COUNT][SLOT_CAP];
  // Pads the record out to whole flash pages.
  //
  // Not cosmetic. Flash programs in pages, so a write rounds its length up — and
  // rounding up a struct that is not a multiple of the page size means the write
  // reads past the end of it and commits whatever static memory happens to
  // follow. Here that was `plain`, the buffer holding the *decrypted* keys, so
  // every commit appended a slice of plaintext to the ciphertext it had just
  // written. Found by dumping the region and searching it, which is the only way
  // this is ever found.
  uint8_t pad[FLASH_PAGE_SIZE -
              ((4 + 4 * STORAGE_SLOT_COUNT + STORAGE_NONCE_LEN + STORAGE_TAG_LEN + 4 +
                STORAGE_SLOT_COUNT * SLOT_CAP) % FLASH_PAGE_SIZE)];
} storage_record_t;

_Static_assert(sizeof(storage_record_t) % FLASH_PAGE_SIZE == 0,
               "the record must be a whole number of flash pages, or committing "
               "it writes adjacent memory to flash");

_Static_assert(sizeof(storage_record_t) <= STORAGE_REGION_SIZE,
               "storage record must fit the reserved region");

static storage_record_t staged;
// The decrypted copy. storage_get() used to hand out a pointer into flash, which
// stops being possible once what is in flash is ciphertext — so the plaintext
// lives here, in RAM, and goes away with the power.
static storage_record_t plain;
static bool loaded;

static const storage_record_t *flash_record(void) {
  return (const storage_record_t *)(XIP_BASE + STORAGE_FLASH_OFFSET);
}

static uint32_t crc32(const void *data, size_t length) {
  const uint8_t *bytes = data;
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; i++) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

static uint32_t record_crc(const storage_record_t *record) {
  // Everything except the crc field itself.
  uint32_t crc = crc32(&record->magic, sizeof(record->magic));
  crc ^= crc32(record->length, sizeof(record->length));
  crc ^= crc32(record->blob, sizeof(record->blob));
  return crc;
}

static bool record_valid(const storage_record_t *record) {
  if (record->magic != STORAGE_MAGIC) return false;
  for (int i = 0; i < STORAGE_SLOT_COUNT; i++) {
    if (record->length[i] == 0 || record->length[i] >= SLOT_CAP) return false;
  }
  return record->crc == record_crc(record);
}

// Derives the wrapping key from the device secret.
//
// Hashed with a label rather than used raw, so something else needing a key
// later gets its own without a second OTP page, and so the value in OTP is never
// itself an AES key.
static bool wrapping_key(uint8_t out[32]) {
  uint8_t secret[OTP_SECRET_LEN];
  if (!otp_secret_read(secret)) return false;

  static const char label[] = "openhanko-storage-v1";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, secret, sizeof(secret));
  mbedtls_sha256_update(&ctx, (const uint8_t *)label, sizeof(label) - 1);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);

  memset(secret, 0, sizeof(secret));
  return true;
}

// Header fields that are authenticated but not encrypted.
static void aad_of(const storage_record_t *r, uint8_t *out) {
  memcpy(out, &r->magic, 4);
  memcpy(out + 4, r->length, 4 * STORAGE_SLOT_COUNT);
}

// Decrypts flash into `plain`. False if there is nothing there, if the record
// predates encryption, or if the tag does not verify — the last covering both a
// tampered record and a device holding the wrong secret, which from here are the
// same thing.
static bool decrypt_into_plain(void) {
  const storage_record_t *r = flash_record();
  memset(&plain, 0, sizeof(plain));
  if (!record_valid(r)) return false;

  uint8_t key[32];
  if (!wrapping_key(key)) {
    printf("storage: no device secret; the stored identity cannot be unwrapped\n");
    return false;
  }

  uint8_t aad[4 + 4 * STORAGE_SLOT_COUNT];
  aad_of(r, aad);

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&gcm, sizeof(r->blob),
                                  r->nonce, sizeof(r->nonce),
                                  aad, sizeof(aad),
                                  r->tag, sizeof(r->tag),
                                  (const unsigned char *)r->blob,
                                  (unsigned char *)plain.blob);
  }
  mbedtls_gcm_free(&gcm);
  memset(key, 0, sizeof(key));

  if (rc != 0) {
    printf("storage: the stored identity did not authenticate (rc=%d)\n", rc);
    memset(&plain, 0, sizeof(plain));
    return false;
  }
  memcpy(plain.length, r->length, sizeof(plain.length));
  return true;
}

void storage_init(void) {
  loaded = decrypt_into_plain();
}

bool storage_loaded(void) {
  return loaded;
}

const char *storage_get(storage_slot_t slot) {
  if (!loaded || slot >= STORAGE_SLOT_COUNT) return NULL;
  return plain.blob[slot];
}

void storage_stage_reset(void) {
  memset(&staged, 0, sizeof(staged));
  staged.magic = STORAGE_MAGIC;
}

bool storage_stage_append(storage_slot_t slot, const void *data, size_t length) {
  if (slot >= STORAGE_SLOT_COUNT) return false;
  uint32_t used = staged.length[slot];
  if (used + length + 1 > SLOT_CAP) return false;
  memcpy(staged.blob[slot] + used, data, length);
  staged.length[slot] = used + (uint32_t)length;
  staged.blob[slot][staged.length[slot]] = '\0';
  return true;
}

bool storage_stage_commit(void) {
  for (int i = 0; i < STORAGE_SLOT_COUNT; i++) {
    if (staged.length[i] == 0) return false;
  }
  // "PRIVATE KEY" rather than "BEGIN PRIVATE KEY", because the two sources of a
  // key here disagree on the header. provision.py sends PKCS#8, which begins
  // "-----BEGIN PRIVATE KEY-----"; mbedtls_pk_write_key_pem emits SEC1 for EC
  // keys, "-----BEGIN EC PRIVATE KEY-----", which does not contain the longer
  // string. Demanding the exact PKCS#8 header rejected every key the device
  // generated for itself, and did so quietly — the only complaint went to a
  // UART that is not connected on a finished unit.
  if (!strstr(staged.blob[STORAGE_CERT_9A], "BEGIN CERTIFICATE") ||
      !strstr(staged.blob[STORAGE_KEY_9A], "PRIVATE KEY") ||
      !strstr(staged.blob[STORAGE_CERT_9D], "BEGIN CERTIFICATE") ||
      !strstr(staged.blob[STORAGE_KEY_9D], "PRIVATE KEY")) {
    return false;
  }
  // Encrypt in place: `staged` is discarded after the write, and the plaintext
  // is recovered by decrypting what actually landed rather than by keeping a
  // second copy — which also proves the record round-trips before anything
  // depends on it.
  uint8_t key[32];
  if (!wrapping_key(key)) {
    printf("storage: refusing to store an identity with no device secret\n");
    return false;
  }
  for (size_t i = 0; i < sizeof(staged.nonce); i += sizeof(uint64_t)) {
    uint64_t r = get_rand_64();
    size_t n = sizeof(staged.nonce) - i;
    if (n > sizeof(r)) n = sizeof(r);
    memcpy(staged.nonce + i, &r, n);
  }

  uint8_t aad[4 + 4 * STORAGE_SLOT_COUNT];
  aad_of(&staged, aad);

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, sizeof(staged.blob),
                                   staged.nonce, sizeof(staged.nonce),
                                   aad, sizeof(aad),
                                   (const unsigned char *)staged.blob,
                                   (unsigned char *)staged.blob,
                                   sizeof(staged.tag), staged.tag);
  }
  mbedtls_gcm_free(&gcm);
  memset(key, 0, sizeof(key));
  if (rc != 0) {
    printf("storage: could not wrap the identity (rc=%d)\n", rc);
    return false;
  }

  staged.crc = record_crc(&staged);

  // Flash writes must be a multiple of the page size, and nothing may execute
  // from flash while the operation runs.
  // Exactly the struct now that it is page-sized; no rounding, so nothing
  // outside it is read.
  size_t span = sizeof(staged);

  uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(STORAGE_FLASH_OFFSET, STORAGE_REGION_SIZE);
  flash_range_program(STORAGE_FLASH_OFFSET, (const uint8_t *)&staged, span);
  restore_interrupts(interrupts);

  // Read it back through the same path a boot takes, so a record that cannot be
  // unwrapped is discovered now rather than at the next power-up.
  memset(&staged, 0, sizeof(staged));
  loaded = decrypt_into_plain();
  return loaded;
}

bool storage_erase(void) {
  uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(STORAGE_FLASH_OFFSET, STORAGE_REGION_SIZE);
  restore_interrupts(interrupts);
  storage_stage_reset();
  loaded = false;
  return true;
}

storage_slot_t storage_slot_by_name(const char *name) {
  if (strcmp(name, "cert9a") == 0) return STORAGE_CERT_9A;
  if (strcmp(name, "key9a") == 0) return STORAGE_KEY_9A;
  if (strcmp(name, "cert9d") == 0) return STORAGE_CERT_9D;
  if (strcmp(name, "key9d") == 0) return STORAGE_KEY_9D;
  return STORAGE_SLOT_COUNT;
}
