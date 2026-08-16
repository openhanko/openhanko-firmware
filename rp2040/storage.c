#include "storage.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define STORAGE_MAGIC 0x53434B31u  // "SCK1"
#define SLOT_CAP 2560

// Occupies the last three erase sectors, which is what the record needs once
// rounded up. Kept at the top of flash and outside the image, so a firmware
// update does not disturb a provisioned identity — the same property NVS gives
// on the ESP32 build. PICO_FLASH_SIZE_BYTES comes from the board header, so
// this follows the part rather than assuming 2 MB.
#define STORAGE_REGION_SIZE (3 * FLASH_SECTOR_SIZE)
#define STORAGE_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - STORAGE_REGION_SIZE)

typedef struct {
  uint32_t magic;
  uint32_t length[STORAGE_SLOT_COUNT];
  uint32_t crc;
  char blob[STORAGE_SLOT_COUNT][SLOT_CAP];
} storage_record_t;

_Static_assert(sizeof(storage_record_t) <= STORAGE_REGION_SIZE,
               "storage record must fit the reserved region");

static storage_record_t staged;
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

void storage_init(void) {
  loaded = record_valid(flash_record());
}

bool storage_loaded(void) {
  return loaded;
}

const char *storage_get(storage_slot_t slot) {
  if (!loaded || slot >= STORAGE_SLOT_COUNT) return NULL;
  return flash_record()->blob[slot];
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
  if (!strstr(staged.blob[STORAGE_CERT_9A], "BEGIN CERTIFICATE") ||
      !strstr(staged.blob[STORAGE_KEY_9A], "BEGIN PRIVATE KEY") ||
      !strstr(staged.blob[STORAGE_CERT_9D], "BEGIN CERTIFICATE") ||
      !strstr(staged.blob[STORAGE_KEY_9D], "BEGIN PRIVATE KEY")) {
    return false;
  }
  staged.crc = record_crc(&staged);

  // Flash writes must be a multiple of the page size, and nothing may execute
  // from flash while the operation runs.
  size_t span = (sizeof(staged) + FLASH_PAGE_SIZE - 1) & ~(size_t)(FLASH_PAGE_SIZE - 1);

  uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(STORAGE_FLASH_OFFSET, STORAGE_REGION_SIZE);
  flash_range_program(STORAGE_FLASH_OFFSET, (const uint8_t *)&staged, span);
  restore_interrupts(interrupts);

  loaded = record_valid(flash_record());
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
