#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// BLE transport for the PIV applet.
//
// macOS has no Bluetooth smart-card transport, so this is not a card the system
// will find on its own — it is the wire under a CryptoTokenKit token driver,
// which is the only place a third party can teach macOS about a new transport.
//
// The protocol is deliberately thin: the host writes an APDU, the device
// answers with the applet's response. piv_handle_apdu() is reused untouched, so
// wired and wireless share one implementation and cannot drift apart.
//
// Framing, because a GATT MTU is smaller than some responses:
//
//     [uint16 total length big-endian][payload ...]
//
// split across writes/notifications of at most (MTU - 3) bytes.

typedef bool (*ble_apdu_handler_t)(const uint8_t *apdu, size_t apdu_len,
                                   uint8_t *response, size_t *response_len,
                                   size_t response_cap);

void ble_transport_start(ble_apdu_handler_t handler);

// True while a host is connected.
bool ble_transport_connected(void);
