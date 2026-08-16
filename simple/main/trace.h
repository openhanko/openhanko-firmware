#pragma once

#include <stdint.h>

// Diagnostic ring buffer covering both layers of the card interface.
//
// It exists to answer one question: does macOS tell the card anything before it
// puts a PIN prompt on screen? APDUs alone say no, so the CCID transport is
// recorded too — power-on, slot-status polls and the rest.
//
// Written from the TinyUSB task, read from the console task. A torn read only
// ever garbles a diagnostic line.

void trace_apdu(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2, uint16_t sw);
void trace_ccid(uint8_t msg_type);
void trace_event(const char *name);
// Like trace_event, but carries a numeric detail: a NimBLE disconnect reason,
// a notify error, and so on. `name` must be a literal; only the pointer is kept.
void trace_ble(const char *name, int code);
void trace_clear(void);
void trace_dump(void (*emit)(const char *line));
