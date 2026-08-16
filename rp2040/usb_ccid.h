#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*ccid_apdu_handler_t)(const uint8_t *apdu, size_t apdu_len,
                                    uint8_t *response, size_t *response_len,
                                    size_t response_cap);

void usb_ccid_start(ccid_apdu_handler_t handler);

// Pinpad PIN entry.
//
// When the host delegates PIN collection to the reader it sends
// PC_to_RDR_Secure and then waits. Answering that means waiting for a button
// press, which is far too long to sit inside the USB callback, so the reply is
// deferred: the main loop drives these while the request is outstanding.
bool usb_ccid_pin_pending(void);

// Keeps the host waiting with CCID time extensions, and gives up on timeout.
void usb_ccid_pin_tick(void);

// Answers the outstanding request. Approved means the press happened.
void usb_ccid_pin_complete(bool approved);
