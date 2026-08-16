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
void trace_clear(void);
void trace_dump(void (*emit)(const char *line));
