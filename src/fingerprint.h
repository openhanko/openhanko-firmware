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

// Re-runs the power-up and password handshakes and re-reads the template count.
// For bring-up: rewire, probe, get an answer, with no reflash in between.
bool fingerprint_probe(void);

// Which baud rate the module answered on during the last probe, 0 if none.
uint32_t fingerprint_probe_baud(void);

// Evidence from the last probe: how many bytes arrived on the module's TX line
// at all, and whether the 0x55 power-up handshake was among them.
uint16_t fingerprint_probe_rx_bytes(void);
bool fingerprint_probe_saw_hello(void);

// The same, from the probe run at boot — which is the one that can catch the
// module's unprompted 0x55 and so prove its TX reaches us.
uint16_t fingerprint_boot_rx_bytes(void);
bool fingerprint_boot_saw_hello(void);

// How the two UART pins looked as plain inputs at boot, before the UART took
// them: samples seen high out of 6000, and how many times the level changed.
// Pass true for the RX pin, false for TX.
uint16_t fingerprint_line_high(bool rx_pin);
uint16_t fingerprint_line_edges(bool rx_pin);

// Narrowest pulse measured on the RX line at boot, in microseconds: one bit
// time, so 1e6 divided by it is the baud the module is really using.
uint32_t fingerprint_line_min_pulse_us(void);

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

// PS_AutoEnroll. The module runs the whole sequence and enforces its own rules:
// it refuses a finger already enrolled, and refuses to overwrite an occupied
// slot. `entries` is how many impressions it asks for.
bool fingerprint_auto_enroll(uint16_t slot, uint8_t entries, uint32_t timeout_ms);

// The confirmation code the last enrolment ended on, so a caller can tell
// "already enrolled" from a genuine failure.
uint8_t fingerprint_last_enroll_cc(void);

// Erases every template. The fingerprint half of a factory reset.
bool fingerprint_erase_all(void);

// Drives the module's own LED ring. This is the ZW1xx AURALEDCONFIG command and
// is not available on the ZW06xx/ZW09xx parts, which is one reason this project
// specifies a ZW111.
bool fingerprint_light(fp_light_t effect, fp_color_t color, uint8_t cycles);

// "absent", or the enrolled template count, for STATUS.
const char *fingerprint_status_text(void);

// Draws 4 bytes from the module's hardware RNG. Worth having as a second source
// to mix in — but only ever mixed, never trusted alone: this is an opaque module
// from a vendor nobody has audited.
bool fingerprint_random(uint32_t *out);

// Four 8-byte ASCII fields out of the module's info page. Padded strings are
// trimmed; any field the module leaves blank comes back empty.
#define FP_CHIP_SERIAL_LEN 32

typedef struct {
  // The manual calls this Product SN and defines it as "indicate product
  // model" — it names the part, not the unit, so it is no use for binding.
  // fingerprint_chip_serial() is the per-unit identity.
  char product_model[9];
  char sw_version[9];
  char manufacturer[9];
  char sensor_name[9];
  uint32_t device_address;   // 0xFFFFFFFF from the factory
  uint16_t capacity;         // templates the module can hold
  uint32_t baud;             // already multiplied out
  uint16_t security_level;   // 0 = safety instruction set unavailable
  uint32_t password;         // readable, which is why it is not a secret
  uint16_t table_flag;       // 0x1234 once the parameter table is initialised
} fp_info_t;

// PS_ReadINFpage (0x16): reads the module's 512-byte info page.
//
// The manual documents the fields and their lengths but not their offsets, so
// the offsets in the implementation were recovered from a real module and are
// corroborated at four independent points. Diagnostic only — bind to
// fingerprint_chip_serial() instead, which identifies the die rather than the
// model.
bool fingerprint_read_info(fp_info_t *out);

// The info page as the module sent it, for working out where the fields
// actually live. Returns bytes stored.
uint16_t fingerprint_read_info_page(uint8_t *out, uint16_t cap);

// PS_GetChipSN: the die's unique 32-byte serial. This is what a device should
// bind to if it is to notice its sensor being swapped — unlike the info page's
// Product SN, which the manual defines as a model identifier.
bool fingerprint_chip_serial(uint8_t out[FP_CHIP_SERIAL_LEN]);

// Whether the module's TouchOut line is wired on this board, and what it says
// right now. Reported by STATUS, which is the first place to look if a board
// stops authenticating: the correlation in fingerprint_verify() refuses a match
// that arrives while this line says nothing is touching the sensor, so a broken
// wire presents as a sensor that lights up and never accepts a finger.
bool fingerprint_touch_wired(void);
bool fingerprint_touch_asserted(void);
