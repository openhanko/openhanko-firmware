#include "settings.h"

#include <string.h>

#include "board_config.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define SETTINGS_MAGIC 0x53434647u  // "SCFG"

// One sector, immediately below the identity region so both stay at the top of
// flash and outside the image. Erasing one does not disturb the other.
#define SETTINGS_REGION_SIZE FLASH_SECTOR_SIZE
#define SETTINGS_FLASH_OFFSET \
  (PICO_FLASH_SIZE_BYTES - (3 * FLASH_SECTOR_SIZE) - SETTINGS_REGION_SIZE)

typedef struct {
  uint32_t magic;
  uint32_t aid_mode;
  // The fingerprint module this device was set up with, from PS_GetChipSN.
  //
  // Not a secret — anyone holding the module can read it — so it is stored
  // whole rather than hashed. Its only job is to notice that the module is not
  // the one that was here before, which is what someone swapping in a sensor
  // they control would have to hide.
  uint8_t module_serial[SETTINGS_SERIAL_LEN];
  uint32_t module_bound;   // 0 until a module has been recorded
  uint32_t crc;
} settings_record_t;

_Static_assert(sizeof(settings_record_t) <= SETTINGS_REGION_SIZE,
               "settings record must fit its sector");

static aid_mode_t cached_mode;
static bool cached_bound;
static uint8_t cached_serial[SETTINGS_SERIAL_LEN];

static const settings_record_t *flash_record(void) {
  return (const settings_record_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
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

static uint32_t record_crc(const settings_record_t *record) {
  uint32_t crc = crc32(&record->magic, sizeof(record->magic));
  crc ^= crc32(&record->aid_mode, sizeof(record->aid_mode));
  crc ^= crc32(record->module_serial, sizeof(record->module_serial));
  crc ^= crc32(&record->module_bound, sizeof(record->module_bound));
  return crc;
}

static bool record_valid(const settings_record_t *record) {
  return record->magic == SETTINGS_MAGIC && record->crc == record_crc(record) &&
         record->aid_mode <= AID_MODE_PINPAD;
}

void settings_init(void) {
  const settings_record_t *record = flash_record();
  // Erased flash reads as 0xff, so an unwritten sector fails the magic check
  // and falls back to the default — which is what a factory-fresh device wants.
  if (record_valid(record)) {
    cached_mode = (aid_mode_t)record->aid_mode;
    cached_bound = record->module_bound != 0;
    memcpy(cached_serial, record->module_serial, sizeof(cached_serial));
  } else {
    cached_mode = (aid_mode_t)PIV_DEFAULT_AID_MODE;
    cached_bound = false;
    memset(cached_serial, 0, sizeof(cached_serial));
  }
}

bool settings_module_bound(void) { return cached_bound; }

const uint8_t *settings_module_serial(void) { return cached_serial; }

// Writes the whole record, so callers that change one field keep the others.
static bool commit(aid_mode_t mode, bool bound, const uint8_t *serial) {
  settings_record_t record = {
      .magic = SETTINGS_MAGIC,
      .aid_mode = (uint32_t)mode,
      .module_bound = bound ? 1u : 0u,
  };
  if (serial) memcpy(record.module_serial, serial, sizeof(record.module_serial));
  record.crc = record_crc(&record);

  uint8_t page[FLASH_PAGE_SIZE];
  memset(page, 0xff, sizeof(page));
  memcpy(page, &record, sizeof(record));

  uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(SETTINGS_FLASH_OFFSET, SETTINGS_REGION_SIZE);
  flash_range_program(SETTINGS_FLASH_OFFSET, page, sizeof(page));
  restore_interrupts(interrupts);

  if (!record_valid(flash_record())) return false;
  cached_mode = mode;
  cached_bound = bound;
  if (serial) memcpy(cached_serial, serial, sizeof(cached_serial));
  return true;
}

bool settings_bind_module(const uint8_t *serial) {
  return commit(cached_mode, true, serial);
}

aid_mode_t settings_aid_mode(void) {
  return cached_mode;
}

bool settings_set_aid_mode(aid_mode_t mode) {
  if (mode == cached_mode) return true;
  // Carries the module binding through: a mode change must not silently unbind
  // the sensor, which writing only this field would do.
  return commit(mode, cached_bound, cached_serial);
}

bool settings_reset(void) {
  uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(SETTINGS_FLASH_OFFSET, SETTINGS_REGION_SIZE);
  restore_interrupts(interrupts);
  cached_mode = (aid_mode_t)PIV_DEFAULT_AID_MODE;
  return true;
}

const char *settings_aid_mode_name(aid_mode_t mode) {
  return mode == AID_MODE_PINPAD ? "pinpad" : "standard";
}
