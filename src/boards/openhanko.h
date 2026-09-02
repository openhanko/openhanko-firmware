// Board definition for the OpenHanko OH-01.
//
// The part is an RP2354A: an RP2350A die with 2 MB of flash stacked in the same
// package. There is no stock SDK board for it — pico2 is the closest and is
// wrong in the one way that matters, because a Pico 2 carries 4 MB in a separate
// chip.
//
// That mattered more than a wrong constant usually does. storage.c and
// settings.c place their regions at the top of flash, derived from
// PICO_FLASH_SIZE_BYTES, so a 4 MB assumption put the identity at 0x3fd000 and
// the settings at 0x3fc000 — addresses this part does not have. It appeared to
// work because a QSPI device ignores address bits above its own size, so both
// aliased quietly down to 0x1fd000 and 0x1fc000. Correct by accident, on
// behaviour no datasheet promises, and silently wrong the moment anything else
// is placed by the same arithmetic.

#ifndef _BOARDS_OPENHANKO_H
#define _BOARDS_OPENHANKO_H

// pico_cmake_set PICO_PLATFORM=rp2350
// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (2 * 1024 * 1024)

#define PICO_RP2350A 1

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

// A2 is the stepping whose errata this design refuses to live with: glitch to
// debug and OTP, unsigned boot, laser fault. The firmware reports the stepping
// it finds and this build does not carry the A2 workarounds.
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 0
#endif

// Diagnostics only, and not connected on an assembled unit — the enclosure is
// sealed and these pads are unpopulated. GP4 and GP5 belong to the fingerprint
// module, so the default UART pins are moved clear of them.
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// No discrete LED: the fingerprint module's ring is the entire indicator.
// PICO_DEFAULT_LED_PIN is deliberately left undefined.

#endif
