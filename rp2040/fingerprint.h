#pragma once

#include <stdbool.h>
#include <stdint.h>

// HLK-ZW111 capacitive fingerprint module, over UART.
//
// Speaks the EF-01 packet protocol shared by the Hi-Link ZW series and the
// older AS608/R307 modules, so this also works with an HLK-ZW101 — useful,
// since upstream tinyTouch targets that part and it is the reference to compare
// against when something behaves oddly.
//
// The module matches on its own hardware and reports a verdict over the wire.
// That makes the UART the trust boundary: anyone who can open the case and
// reach these two pins can assert a match. It is worth being clear-eyed that
// this does not make the device tamper-proof — what makes the fingerprint
// meaningful is the key being unreadable, which is a property of the MCU
// (RP2350 OTP and locked debug), not of this sensor.
//
// Everything here degrades to absence. A device with no sensor wired answers
// fingerprint_present() with false and behaves exactly as it did before, so one
// firmware serves both the button prototypes and the finished units.

typedef enum {
  FP_LED_OFF = 0,
  FP_LED_BLUE,
  FP_LED_GREEN,
  FP_LED_CYAN,
  FP_LED_RED,
  FP_LED_PURPLE,
  FP_LED_YELLOW,
  FP_LED_WHITE,
} fp_color_t;

typedef enum {
  FP_LIGHT_BREATHE = 1,
  FP_LIGHT_FLASH = 2,
  FP_LIGHT_STEADY = 3,
  FP_LIGHT_OFF = 4,
  FP_LIGHT_FADE_IN = 5,
  FP_LIGHT_FADE_OUT = 6,
} fp_light_t;

// Probes for the module. Safe to call with nothing attached.
void fingerprint_init(void);

// True when a module answered the handshake at startup.
bool fingerprint_present(void);

// How many templates are enrolled, or 0 if unknown.
uint16_t fingerprint_template_count(void);

// True when a finger is on the sensor right now. Cheap enough to call from the
// main loop: with no finger the module answers immediately.
bool fingerprint_finger_down(void);

// Captures and searches against the enrolled templates. Returns true only on a
// match, and writes the matching slot and the module's confidence score when
// pointers are supplied. Takes a few hundred milliseconds and pumps USB while
// it waits, so it may be called from the cooperative main loop.
bool fingerprint_verify(uint16_t *slot, uint16_t *score);

// Enrolls the finger currently being presented into `slot`, taking two
// impressions. Blocks for as long as it takes the user to comply, up to
// `timeout_ms`, pumping USB throughout.
bool fingerprint_enroll(uint16_t slot, uint32_t timeout_ms);

// Erases every template. The fingerprint half of a factory reset.
bool fingerprint_erase_all(void);

// Drives the module's own LED ring. This is the ZW1xx AURALEDCONFIG command and
// is not available on the ZW06xx/ZW09xx parts, which is one reason this project
// specifies a ZW111.
bool fingerprint_light(fp_light_t effect, fp_color_t color, uint8_t cycles);

// "absent", or the enrolled template count, for STATUS.
const char *fingerprint_status_text(void);

// Draws 4 bytes from the module's hardware RNG. Worth having on the RP2040,
// whose own randomness is ring-oscillator jitter — but mix it, never trust it
// alone: this is an opaque module from a vendor nobody has audited.
bool fingerprint_random(uint32_t *out);

// Four 8-byte ASCII fields out of the module's info page. Padded strings are
// trimmed; any field the module leaves blank comes back empty.
typedef struct {
  char product_sn[9];
  char sw_version[9];
  char manufacturer[9];
  char sensor_name[9];
} fp_info_t;

// PS_ReadINFpage (0x16): reads the module's 512-byte info page.
//
// UNTESTED against hardware, and the field offsets are a guess in a specific
// way — see the note in fingerprint.c. Intended for binding the device to one
// module, but whether that is possible at all depends on product_sn being
// per-unit rather than per-model, which is exactly what nobody has confirmed.
bool fingerprint_read_info(fp_info_t *out);

// The info page as the module sent it, for working out where the fields
// actually live. Returns bytes stored.
uint16_t fingerprint_read_info_page(uint8_t *out, uint16_t cap);
