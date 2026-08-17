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
  uint32_t crc;
} settings_record_t;

_Static_assert(sizeof(settings_record_t) <= SETTINGS_REGION_SIZE,
               "settings record must fit its sector");

static aid_mode_t cached_mode;

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
  cached_mode = record_valid(record) ? (aid_mode_t)record->aid_mode
                                     : (aid_mode_t)PIV_DEFAULT_AID_MODE;
}

aid_mode_t settings_aid_mode(void) {
  return cached_mode;
}

bool settings_set_aid_mode(aid_mode_t mode) {
  if (mode == cached_mode) return true;

  settings_record_t record = {
      .magic = SETTINGS_MAGIC,
      .aid_mode = (uint32_t)mode,
  };
  record.crc = record_crc(&record);

  uint8_t page[FLASH_PAGE_SIZE];
  memset(page, 0xff, sizeof(page));
  memcpy(page, &record, sizeof(record));

  // Nothing may execute from flash while this runs.
  uint32_t interrupts = save_and_disable_interrupts();
  flash_range_erase(SETTINGS_FLASH_OFFSET, SETTINGS_REGION_SIZE);
  flash_range_program(SETTINGS_FLASH_OFFSET, page, sizeof(page));
  restore_interrupts(interrupts);

  // Read back rather than trust the write. A mode change is normally followed
  // by a reboot, and rebooting into an unwritten setting would look like the
  // command was ignored.
  if (!record_valid(flash_record()) || flash_record()->aid_mode != (uint32_t)mode) {
    return false;
  }
  cached_mode = mode;
  return true;
}

const char *settings_aid_mode_name(aid_mode_t mode) {
  return mode == AID_MODE_PINPAD ? "pinpad" : "standard";
}
