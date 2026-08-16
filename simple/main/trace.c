#include "trace.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TRACE_DEPTH 96

typedef enum {
  TRACE_KIND_APDU = 0,
  TRACE_KIND_CCID,
  TRACE_KIND_EVENT,
} trace_kind_t;

typedef struct {
  uint32_t ms;
  uint8_t kind;
  uint8_t cla, ins, p1, p2;
  uint16_t sw;
  uint16_t repeat;
  const char *name;
} trace_entry_t;

static trace_entry_t ring[TRACE_DEPTH];
static size_t next_slot;
static size_t used;

static uint32_t now_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static trace_entry_t *last_entry(void) {
  if (used == 0) return NULL;
  return &ring[(next_slot + TRACE_DEPTH - 1) % TRACE_DEPTH];
}

static trace_entry_t *claim_slot(void) {
  trace_entry_t *entry = &ring[next_slot];
  memset(entry, 0, sizeof(*entry));
  next_slot = (next_slot + 1) % TRACE_DEPTH;
  if (used < TRACE_DEPTH) used++;
  entry->ms = now_ms();
  entry->repeat = 1;
  return entry;
}

void trace_apdu(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2, uint16_t sw) {
  trace_entry_t *entry = claim_slot();
  entry->kind = TRACE_KIND_APDU;
  entry->cla = cla;
  entry->ins = ins;
  entry->p1 = p1;
  entry->p2 = p2;
  entry->sw = sw;
}

void trace_ccid(uint8_t msg_type) {
  // Slot-status polling repeats forever; collapse runs of it so a few seconds
  // of idle does not evict everything interesting from the ring.
  trace_entry_t *previous = last_entry();
  if (previous && previous->kind == TRACE_KIND_CCID && previous->ins == msg_type &&
      previous->repeat < 0xffff) {
    previous->repeat++;
    previous->ms = now_ms();
    return;
  }
  trace_entry_t *entry = claim_slot();
  entry->kind = TRACE_KIND_CCID;
  entry->ins = msg_type;
}

void trace_event(const char *name) {
  trace_entry_t *entry = claim_slot();
  entry->kind = TRACE_KIND_EVENT;
  entry->name = name;
}

void trace_clear(void) {
  next_slot = 0;
  used = 0;
}

static const char *ccid_name(uint8_t type) {
  switch (type) {
    case 0x62: return "IccPowerOn";
    case 0x63: return "IccPowerOff";
    case 0x65: return "GetSlotStatus";
    case 0x6f: return "XfrBlock";
    case 0x61: return "SetParameters";
    case 0x6c: return "GetParameters";
    case 0x6d: return "ResetParameters";
    default: return "other";
  }
}

void trace_dump(void (*emit)(const char *line)) {
  char line[112];
  size_t start = (next_slot + TRACE_DEPTH - used) % TRACE_DEPTH;
  for (size_t i = 0; i < used; i++) {
    const trace_entry_t *entry = &ring[(start + i) % TRACE_DEPTH];
    switch (entry->kind) {
      case TRACE_KIND_EVENT:
        snprintf(line, sizeof(line), "TRACE %lu EVENT %s",
                 (unsigned long)entry->ms, entry->name ? entry->name : "?");
        break;
      case TRACE_KIND_CCID:
        snprintf(line, sizeof(line), "TRACE %lu CCID %02x %s x%u",
                 (unsigned long)entry->ms, entry->ins, ccid_name(entry->ins),
                 (unsigned)entry->repeat);
        break;
      default:
        snprintf(line, sizeof(line),
                 "TRACE %lu APDU cla=%02x ins=%02x p1=%02x p2=%02x sw=%04x",
                 (unsigned long)entry->ms, entry->cla, entry->ins,
                 entry->p1, entry->p2, entry->sw);
        break;
    }
    emit(line);
  }
}
