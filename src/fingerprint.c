#include "fingerprint.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include "tusb.h"

#if FINGERPRINT_UART_TX >= 0

// EF-01 packet protocol.
//
//   EF 01 | FF FF FF FF | PID | len_hi len_lo | INS params… | cs_hi cs_lo
//
// The length field counts the body *plus* the two checksum bytes, and the
// checksum covers the PID and both length bytes as well as the body. Both are
// easy to get subtly wrong and produce a module that simply never answers.
#define PID_COMMAND 0x01
#define PID_ACK     0x07
// A payload larger than one packet arrives as a run of PID_DATA packets closed
// by a single PID_END. Only PS_ReadINFpage uses this here.
#define PID_DATA    0x02
#define PID_END     0x08

#define INS_GET_IMAGE       0x01
#define INS_IMAGE_TO_BUFFER 0x02
#define INS_CREATE_MODEL    0x05
#define INS_STORE_TEMPLATE  0x06
#define INS_ERASE_ALL       0x0d
#define INS_VERIFY_PASSWORD 0x13
#define INS_GET_RANDOM      0x14
// PS_Search. Was 0x1b, taken from a third-party library that calls it a
// "high-speed search" — Hi-Link's protocol manual has no such command, and the
// only 0x1b in it is an error code. Left alone, every verification would have
// failed against a real module for a reason that looks like a broken sensor.
#define INS_SEARCH          0x04
// PS_GetChipSN: 32 bytes, unique per die.
#define INS_CHIP_SERIAL     0x34
#define INS_HANDSHAKE       0x35
#define INS_AUTO_ENROLL     0x31

// PS_AutoEnroll flags. Bit 2 is left clear on purpose: it asks the module to
// report progress at each step, which is what drives the ring during enrolment.
#define AUTO_ENROLL_FLAGS ( (1u << 0)   /* backlight off once the image is taken */ \
                          | (1u << 1)   /* image preprocessing on */               \
                          | (1u << 4) ) /* refuse a finger already enrolled */

// Progress stages reported in parameter 1.
#define STAGE_GET_IMAGE   0x01
#define STAGE_FEATURE     0x02
#define STAGE_FINGER_AWAY 0x03
#define STAGE_MERGE       0x04
#define STAGE_STORE       0x06

// Confirmation codes worth naming from the auto-enrolment stream.
#define CC_ALREADY_EXISTS 0x27
#define CC_NOT_EMPTY      0x22
#define CC_DB_FULL        0x1f
#define CC_TIMEOUT        0x26
// Safety instruction set, 0xE0-0xE4. Only 0xE2 is issued here, and only to ask
// whether the module knows the opcode at all — see fingerprint_security_probe().
#define INS_SEC_GET_CIPHERTEXT 0xe2

#define INS_TEMPLATE_COUNT  0x1d
#define INS_AURA_LED        0x3c
#define INS_READ_INFO_PAGE  0x16

// Confirmation codes worth naming; the rest are just "not zero".
#define CC_OK            0x00
#define CC_NO_FINGER     0x02
#define CC_CAPTURE_FAIL  0x03
#define CC_NOT_FOUND     0x09

#define FP_UART (FINGERPRINT_UART_INSTANCE)

static bool module_present;

static uint16_t probe_rx_bytes;
static bool probe_saw_hello;
// The boot probe's result, kept separately so a later manual probe cannot
// overwrite it. It is the more meaningful of the two: the module speaks
// unprompted exactly once, with 0x55 at power-up, so bytes seen then prove its
// TX reaches this pin. A re-probe minutes later cannot prove that — silence
// there is equally consistent with our TX never reaching the module at all.
static uint16_t boot_rx_bytes;
static bool boot_saw_hello;
static bool boot_probe_done;

// What the pins looked like before the UART claimed them.
static uint16_t line_high_tx, line_high_rx, line_edges_tx, line_edges_rx;
// Narrowest pulse seen on the RX line, in microseconds. One bit time, so the
// baud rate is its reciprocal — which measures what the module is actually
// doing instead of guessing from a list.
static uint32_t line_min_pulse_us;

// Samples the two UART pins as plain inputs, before uart_init takes them.
//
// This answers a question a UART cannot: the pin functions are fixed in silicon
// — UART1 TX exists only on GP4/8/20/24 and RX only on GP5/9/21/25 — so the
// lines cannot be swapped in firmware to test a suspected miswire. But an idle
// UART transmitter holds its line high, so with a pull-down fitted, whichever
// pin reads high has something driving it. A module wired correctly shows GP5
// high and GP4 low; the reverse means TX and RX are crossed; both low means
// nothing is driving either, which is a power or ground fault rather than a
// wiring one.
static void scan_uart_lines(void) {
  gpio_init(FINGERPRINT_UART_TX);
  gpio_set_dir(FINGERPRINT_UART_TX, GPIO_IN);
  gpio_pull_down(FINGERPRINT_UART_TX);
  gpio_init(FINGERPRINT_UART_RX);
  gpio_set_dir(FINGERPRINT_UART_RX, GPIO_IN);
  gpio_pull_down(FINGERPRINT_UART_RX);
  sleep_ms(2);  // let the pulls settle before believing a level

  bool last_tx = gpio_get(FINGERPRINT_UART_TX);
  bool last_rx = gpio_get(FINGERPRINT_UART_RX);
  // 600 ms covers the module's power-up and its unprompted 0x55, whose
  // alternating bits produce edges that a static level check would miss.
  for (int i = 0; i < 6000; i++) {
    bool tx = gpio_get(FINGERPRINT_UART_TX);
    bool rx = gpio_get(FINGERPRINT_UART_RX);
    if (tx) line_high_tx++;
    if (rx) line_high_rx++;
    if (tx != last_tx) { line_edges_tx++; last_tx = tx; }
    if (rx != last_rx) { line_edges_rx++; last_rx = rx; }
    sleep_us(100);
  }
  // Then time the line. The coarse pass above samples at 100 us, which cannot
  // resolve a 17 us bit at 57600, so this polls as fast as the core allows and
  // keeps the shortest gap between edges.
  line_min_pulse_us = UINT32_MAX;
  uint32_t start = time_us_32();
  bool last = gpio_get(FINGERPRINT_UART_RX);
  uint32_t last_edge = start;
  while (time_us_32() - start < 700000u) {
    bool now = gpio_get(FINGERPRINT_UART_RX);
    if (now != last) {
      uint32_t t = time_us_32();
      uint32_t d = t - last_edge;
      if (d > 0 && d < line_min_pulse_us) line_min_pulse_us = d;
      last_edge = t;
      last = now;
    }
  }
  if (line_min_pulse_us == UINT32_MAX) line_min_pulse_us = 0;

  printf("fingerprint: line scan gp%d high=%u edges=%u, gp%d high=%u edges=%u min=%uus\n",
         FINGERPRINT_UART_TX, line_high_tx, line_edges_tx,
         FINGERPRINT_UART_RX, line_high_rx, line_edges_rx,
         (unsigned)line_min_pulse_us);
}

uint16_t fingerprint_line_high(bool rx_pin) {
  return rx_pin ? line_high_rx : line_high_tx;
}
uint16_t fingerprint_line_edges(bool rx_pin) {
  return rx_pin ? line_edges_rx : line_edges_tx;
}
uint32_t fingerprint_line_min_pulse_us(void) { return line_min_pulse_us; }

// Reads the module's TouchOut line, or falls back to asking the module when the
// line is not wired.
//
// The pin is configured with a pull to the inactive level, so a harness with
// this wire missing reads as "nothing touching" rather than floating between
// the two.
static bool touch_line_asserted(void) {
#if FINGERPRINT_TOUCH_GPIO >= 0
  return gpio_get(FINGERPRINT_TOUCH_GPIO) == (FINGERPRINT_TOUCH_ACTIVE_LEVEL ? 1 : 0);
#else
  return true;  // no line to consult; callers fall back to the UART probe
#endif
}

bool fingerprint_touch_wired(void) {
#if FINGERPRINT_TOUCH_GPIO >= 0
  return true;
#else
  return false;
#endif
}

bool fingerprint_touch_asserted(void) { return touch_line_asserted(); }
static uint16_t template_count;
// Why the last enrolment ended, so the console can say something better than
// "failed": "that finger is already enrolled" is a different user action.
static uint8_t last_enroll_cc;

static uint32_t now_ms(void) {
  return to_ms_since_boot(get_absolute_time());
}

// Waits for one byte, keeping USB alive.
//
// The main loop is cooperative and TinyUSB starves if it is not pumped, so any
// wait longer than a few milliseconds has to service it. A fingerprint search
// takes a few hundred milliseconds, which is far past that line.
static bool read_byte(uint8_t *out, uint32_t deadline) {
  while ((int32_t)(now_ms() - deadline) < 0) {
    if (uart_is_readable(FP_UART)) {
      *out = uart_getc(FP_UART);
      return true;
    }
    tud_task();
    tight_loop_contents();
  }
  return false;
}

static uint16_t checksum(uint8_t pid, uint8_t len_hi, uint8_t len_lo,
                         const uint8_t *body, uint16_t body_len) {
  uint32_t sum = (uint32_t)pid + len_hi + len_lo;
  for (uint16_t i = 0; i < body_len; i++) sum += body[i];
  return (uint16_t)(sum & 0xffff);
}

static void send_command(uint8_t ins, const uint8_t *params, uint8_t param_len) {
  uint8_t body[40];
  body[0] = ins;
  if (param_len && params) memcpy(body + 1, params, param_len);
  uint8_t body_len = 1 + param_len;

  uint16_t length = body_len + 2;  // the field includes the checksum bytes
  uint8_t len_hi = (uint8_t)(length >> 8);
  uint8_t len_lo = (uint8_t)(length & 0xff);
  uint16_t cs = checksum(PID_COMMAND, len_hi, len_lo, body, body_len);

  uint8_t packet[52];
  uint16_t i = 0;
  packet[i++] = 0xef;
  packet[i++] = 0x01;
  packet[i++] = 0xff; packet[i++] = 0xff; packet[i++] = 0xff; packet[i++] = 0xff;
  packet[i++] = PID_COMMAND;
  packet[i++] = len_hi;
  packet[i++] = len_lo;
  memcpy(packet + i, body, body_len); i += body_len;
  packet[i++] = (uint8_t)(cs >> 8);
  packet[i++] = (uint8_t)(cs & 0xff);

  // Drop anything stale before speaking, or a late reply to a timed-out command
  // is read as the answer to this one.
  while (uart_is_readable(FP_UART)) (void)uart_getc(FP_UART);
  uart_write_blocking(FP_UART, packet, i);
}

// Reads one acknowledgement. Returns the confirmation code, or 0xff on timeout
// and 0xfe on a malformed or mis-checksummed packet.
static uint8_t read_ack(uint8_t *payload, uint8_t payload_cap, uint8_t *payload_len,
                        uint32_t timeout_ms) {
  if (payload_len) *payload_len = 0;
  uint32_t deadline = now_ms() + timeout_ms;

  // Hunt for the header rather than assuming alignment: a module reset mid-
  // exchange leaves a partial packet in the pipe.
  uint8_t byte = 0;
  int state = 0;
  while (state < 2) {
    if (!read_byte(&byte, deadline)) return 0xff;
    if (state == 0) state = (byte == 0xef) ? 1 : 0;
    else state = (byte == 0x01) ? 2 : (byte == 0xef ? 1 : 0);
  }

  uint8_t header[7];  // address(4) + pid(1) + length(2)
  for (int i = 0; i < 7; i++) {
    if (!read_byte(&header[i], deadline)) return 0xff;
  }
  uint8_t pid = header[4];
  uint16_t length = ((uint16_t)header[5] << 8) | header[6];
  if (pid != PID_ACK || length < 3 || length > 64) return 0xfe;

  uint16_t body_len = length - 2;
  uint8_t body[64];
  for (uint16_t i = 0; i < body_len; i++) {
    if (!read_byte(&body[i], deadline)) return 0xff;
  }
  uint8_t cs_bytes[2];
  for (int i = 0; i < 2; i++) {
    if (!read_byte(&cs_bytes[i], deadline)) return 0xff;
  }

  uint16_t expected = ((uint16_t)cs_bytes[0] << 8) | cs_bytes[1];
  if (checksum(pid, header[5], header[6], body, body_len) != expected) return 0xfe;

  if (payload && payload_len && body_len > 1) {
    uint8_t copy = (uint8_t)(body_len - 1);
    if (copy > payload_cap) copy = payload_cap;
    memcpy(payload, body + 1, copy);
    *payload_len = copy;
  }
  return body[0];
}

// Reads a PID_DATA/PID_END stream into out, returning bytes stored.
//
// Separate from read_ack() rather than folded into it: read_ack rejects any PID
// that is not an acknowledgement, which is the behaviour every other command
// wants. Only the info page follows its ack with a stream.
static uint16_t read_data_stream(uint8_t *out, uint16_t cap, uint32_t timeout_ms) {
  uint16_t received = 0;
  uint32_t deadline = now_ms() + timeout_ms;

  for (;;) {
    uint8_t byte = 0;
    int state = 0;
    while (state < 2) {
      if (!read_byte(&byte, deadline)) return received;
      if (state == 0) state = (byte == 0xef) ? 1 : 0;
      else state = (byte == 0x01) ? 2 : (byte == 0xef ? 1 : 0);
    }

    uint8_t header[7];
    for (int i = 0; i < 7; i++) {
      if (!read_byte(&header[i], deadline)) return received;
    }
    uint8_t pid = header[4];
    uint16_t length = ((uint16_t)header[5] << 8) | header[6];
    if ((pid != PID_DATA && pid != PID_END) || length < 3) return received;

    uint16_t body_len = length - 2;
    uint8_t body[256];
    if (body_len > sizeof(body)) return received;
    for (uint16_t i = 0; i < body_len; i++) {
      if (!read_byte(&body[i], deadline)) return received;
    }
    uint8_t cs_bytes[2];
    for (int i = 0; i < 2; i++) {
      if (!read_byte(&cs_bytes[i], deadline)) return received;
    }
    uint16_t expected = ((uint16_t)cs_bytes[0] << 8) | cs_bytes[1];
    if (checksum(pid, header[5], header[6], body, body_len) != expected) return received;

    // Store what fits and keep draining: abandoning the stream mid-flight leaves
    // the remaining packets in the pipe to be misread as the next reply.
    if (received < cap) {
      uint16_t copy = body_len;
      if (copy > (uint16_t)(cap - received)) copy = (uint16_t)(cap - received);
      memcpy(out + received, body, copy);
      received += copy;
    }
    if (pid == PID_END) break;
  }
  return received;
}

static uint8_t exchange(uint8_t ins, const uint8_t *params, uint8_t param_len,
                        uint8_t *payload, uint8_t payload_cap, uint8_t *payload_len,
                        uint32_t timeout_ms) {
  send_command(ins, params, param_len);
  return read_ack(payload, payload_cap, payload_len, timeout_ms);
}

// MARK: - Public interface

void fingerprint_init(void) {
  // Before the UART claims the pins, look at what is driving them.
  scan_uart_lines();

  uart_init(FP_UART, FINGERPRINT_BAUD);
  gpio_set_function(FINGERPRINT_UART_TX, GPIO_FUNC_UART);
  gpio_set_function(FINGERPRINT_UART_RX, GPIO_FUNC_UART);
  uart_set_format(FP_UART, 8, 1, UART_PARITY_NONE);
  uart_set_fifo_enabled(FP_UART, true);

#if FINGERPRINT_TOUCH_GPIO >= 0
  // Pulled to the inactive level on purpose: a missing or cut TouchOut then
  // reads as "no finger" and the device refuses to authenticate, rather than
  // floating and occasionally agreeing that someone is there.
  gpio_init(FINGERPRINT_TOUCH_GPIO);
  gpio_set_dir(FINGERPRINT_TOUCH_GPIO, GPIO_IN);
  if (FINGERPRINT_TOUCH_ACTIVE_LEVEL) gpio_pull_down(FINGERPRINT_TOUCH_GPIO);
  else gpio_pull_up(FINGERPRINT_TOUCH_GPIO);
#endif

  bool ok = fingerprint_probe();
  if (!boot_probe_done) {
    boot_probe_done = true;
    boot_rx_bytes = probe_rx_bytes;
    boot_saw_hello = probe_saw_hello;
  }
  if (!ok) {
    printf("fingerprint: no module on uart\n");
    return;
  }

  uint8_t payload[8];
  uint8_t payload_len = 0;
  if (exchange(INS_TEMPLATE_COUNT, NULL, 0, payload, sizeof(payload), &payload_len,
               500) == CC_OK && payload_len >= 2) {
    template_count = ((uint16_t)payload[0] << 8) | payload[1];
  }
  printf("fingerprint: module ready, %u template(s) enrolled\n", template_count);
  fingerprint_light(FP_LIGHT_OFF, FP_LED_OFF, 0);
}

bool fingerprint_present(void) {
  return module_present;
}

uint8_t fingerprint_last_enroll_cc(void) { return last_enroll_cc; }

uint16_t fingerprint_template_count(void) {
  return template_count;
}

// What the last probe saw on the wire, for the console to report. Without this
// the only evidence is a printf on UART0, which a board may not break out.

uint16_t fingerprint_probe_rx_bytes(void) { return probe_rx_bytes; }
bool fingerprint_probe_saw_hello(void) { return probe_saw_hello; }

// The baud the module actually answered on, 0 if none did.
static uint32_t probe_baud;
uint32_t fingerprint_probe_baud(void) { return probe_baud; }

// Tries one baud rate. Both a password verify and a plain handshake, because a
// module whose password has been changed answers VfyPwd with an error rather
// than silence — but only if it can hear us at all, which is the thing being
// tested here.
static bool probe_at_baud(uint32_t baud) {
  uart_set_baudrate(FP_UART, baud);
  sleep_ms(20);
  uint8_t password[4] = {0, 0, 0, 0};
  if (exchange(INS_VERIFY_PASSWORD, password, sizeof(password), NULL, 0, NULL, 400)
      != 0xff) {
    return true;  // anything but a timeout means it heard us
  }
  return exchange(INS_HANDSHAKE, NULL, 0, NULL, 0, NULL, 400) != 0xff;
}

bool fingerprint_probe(void) {
  module_present = false;
  probe_baud = 0;
  template_count = 0;
  probe_rx_bytes = 0;
  probe_saw_hello = false;

  // The manual says the module emits 0x55 on the UART once it has finished
  // initialising, and that a host which does not wait for it should allow
  // 200 ms. Do both: watch for the byte, and fall through on the timeout rather
  // than depend on catching it, since a re-probe long after power-up never will.
  //
  // Counting everything that arrives, not just the 0x55, because the count is
  // the more useful signal during bring-up: nothing at all means the module's TX
  // is not reaching this pin, while bytes that are not 0x55 mean the wire is
  // right and something else — baud, most likely — is not.
  uint32_t deadline = now_ms() + 400;
  uint8_t byte = 0;
  while (now_ms() < deadline) {
    if (!read_byte(&byte, deadline)) break;
    probe_rx_bytes++;
    if (byte == 0x55) { probe_saw_hello = true; break; }
  }

  // Sweep the plausible baud rates rather than trusting the configured one. The
  // module's rate is a stored register — a unit that has been configured before,
  // or that shipped with a different default, answers on one of these and on no
  // other, and the symptom either way is silence.
  static const uint32_t bauds[] = {57600, 9600, 19200, 38400, 115200};
  for (size_t i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) {
    if (probe_at_baud(bauds[i])) { probe_baud = bauds[i]; break; }
  }
  uart_set_baudrate(FP_UART, probe_baud ? probe_baud : (uint32_t)FINGERPRINT_BAUD);
  if (!probe_baud) return false;

  uint8_t password[4] = {0, 0, 0, 0};
  for (int attempt = 0; attempt < 3 && !module_present; attempt++) {
    module_present = exchange(INS_VERIFY_PASSWORD, password, sizeof(password),
                              NULL, 0, NULL, 500) == CC_OK;
  }
  if (!module_present) return false;

  uint8_t payload[8];
  uint8_t payload_len = 0;
  if (exchange(INS_TEMPLATE_COUNT, NULL, 0, payload, sizeof(payload), &payload_len,
               500) == CC_OK && payload_len >= 2) {
    template_count = ((uint16_t)payload[0] << 8) | payload[1];
  }
  printf("fingerprint: module ready, %u template(s) enrolled\n", template_count);
  return true;
}

uint16_t fingerprint_boot_rx_bytes(void) { return boot_rx_bytes; }
bool fingerprint_boot_saw_hello(void) { return boot_saw_hello; }

bool fingerprint_finger_down(void) {
  if (!module_present) return false;
#if FINGERPRINT_TOUCH_GPIO >= 0
  // A GPIO read rather than a capture attempt. Called from the main loop several
  // times a second, this is the difference between a quiet UART and one that is
  // never idle.
  return touch_line_asserted();
#else
  return exchange(INS_GET_IMAGE, NULL, 0, NULL, 0, NULL, 300) == CC_OK;
#endif
}

bool fingerprint_verify(uint16_t *slot, uint16_t *score) {
  if (!module_present || template_count == 0) return false;

#if FINGERPRINT_TOUCH_GPIO >= 0 && FINGERPRINT_REQUIRE_TOUCH
  // A real match requires a finger on the sensor, and a finger on the sensor
  // asserts this line. A match that arrives while it says otherwise was not
  // produced by one — which is what someone gets for driving TX alone after
  // cutting into the harness.
  //
  // Checked here rather than at the call site so every path gets it: the
  // authentication poll, the enrollment gate, and the console.
  if (!touch_line_asserted()) return false;
#endif

  // GET IMAGE must succeed first; it is also how "no finger" is reported.
  uint8_t cc = exchange(INS_GET_IMAGE, NULL, 0, NULL, 0, NULL, 500);
  if (cc != CC_OK) return false;

  uint8_t buffer_index = 1;
  if (exchange(INS_IMAGE_TO_BUFFER, &buffer_index, 1, NULL, 0, NULL, 1000) != CC_OK) {
    // A smudged or partial press lands here. Not an error worth reporting: the
    // user simply presses again.
    return false;
  }

  // PS_Search: buffer id, start page (high/low), page count (high/low).
  uint8_t params[5] = {1, 0x00, 0x00, (uint8_t)(0xff), (uint8_t)(0xff)};
  uint8_t payload[8];
  uint8_t payload_len = 0;
  cc = exchange(INS_SEARCH, params, sizeof(params), payload, sizeof(payload),
                &payload_len, 2000);
  if (cc != CC_OK || payload_len < 4) return false;

#if FINGERPRINT_TOUCH_GPIO >= 0 && FINGERPRINT_REQUIRE_TOUCH
  // And again on the way out. The capture and search above take a few hundred
  // milliseconds, all of which a genuine user spends with a finger on the
  // sensor, so the line should still be asserted. Requiring it at both ends
  // means a forged match has to hold the line for the whole exchange rather
  // than blip it once at the right moment.
  if (!touch_line_asserted()) {
    printf("fingerprint: match discarded, touch line released mid-capture\n");
    return false;
  }
#endif

  if (slot) *slot = ((uint16_t)payload[0] << 8) | payload[1];
  if (score) *score = ((uint16_t)payload[2] << 8) | payload[3];
  return true;
}

// PS_AutoEnroll: the module runs the whole sequence — capture, features, merge,
// store — and reports progress as a stream of acknowledgements.
//
// Better than driving those steps by hand, for reasons about correctness rather
// than tidiness. The impression count becomes a parameter instead of being
// implied by a loop; and the module enforces what the steps mean, refusing a
// finger that is already enrolled and refusing to overwrite an occupied slot,
// neither of which the hand-driven sequence checked.
bool fingerprint_auto_enroll(uint16_t slot, uint8_t entries, uint32_t timeout_ms) {
  if (!module_present) return false;
  uint32_t deadline = now_ms() + timeout_ms;

  uint8_t params[5] = {
    (uint8_t)(slot >> 8), (uint8_t)(slot & 0xff),
    entries,
    (uint8_t)(AUTO_ENROLL_FLAGS >> 8), (uint8_t)(AUTO_ENROLL_FLAGS & 0xff),
  };
  send_command(INS_AUTO_ENROLL, params, sizeof(params));

  uint8_t last_entry = 0;
  for (;;) {
    if ((int32_t)(now_ms() - deadline) >= 0) {
      printf("fingerprint: auto-enrolment timed out\n");
      last_enroll_cc = CC_TIMEOUT;
      goto failed;
    }

    uint8_t payload[4];
    uint8_t payload_len = 0;
    // A long per-packet timeout: between impressions the module is waiting on a
    // person, and silence there is not an error.
    uint8_t cc = read_ack(payload, sizeof(payload), &payload_len, 3000);
    if (cc == 0xff) continue;      // nothing yet; the user is still deciding
    if (cc == 0xfe) goto failed;   // a mangled packet is a real fault
    if (cc != CC_OK) {
      const char *why = cc == CC_ALREADY_EXISTS ? "this finger is already enrolled"
                      : cc == CC_NOT_EMPTY     ? "that slot is occupied"
                      : cc == CC_DB_FULL       ? "the template store is full"
                      : cc == CC_TIMEOUT       ? "the module timed out waiting"
                                               : "refused";
      printf("fingerprint: auto-enrolment failed, cc=%02x (%s)\n", cc, why);
      last_enroll_cc = cc;
      goto failed;
    }
    if (payload_len < 2) continue;

    // The ring is deliberately left alone here. The module lights it itself
    // during auto-enrolment — steady green for an impression taken, blue while
    // it waits for the finger to lift, red on a rejection — and sending our own
    // commands on top produced two schemes fighting over one indicator, which
    // looked like the module misbehaving. It has better information about what
    // it is doing than we do, so during this command it owns the light.
    uint8_t stage = payload[0], detail = payload[1];
    (void)detail;
    if (stage == STAGE_STORE) {
      template_count++;
      last_enroll_cc = CC_OK;
      printf("fingerprint: auto-enrolled slot %u\n", slot);
      return true;
    }
    last_entry = stage;
  }

failed:
  // Also not lit here: the module has already shown its own rejection, and
  // adding ours on top is what made a refusal look like a fault.
  return false;
}

bool fingerprint_enroll(uint16_t slot, uint32_t timeout_ms) {
  if (!module_present) return false;
  uint32_t deadline = now_ms() + timeout_ms;

  // Two impressions of the same finger, with a lift in between. The lift is not
  // politeness: without it the module merges two captures of an identical
  // placement and builds a template that only matches that exact position.
  for (uint8_t pass = 1; pass <= 2; pass++) {
    // The same colour for both impressions, deliberately. This used to breathe
    // blue then purple and flash green after each, which reads as "finger
    // accepted, now the next one" — and a user who obliges presents a second
    // finger, which PS_RegModel merges with the first into a single template of
    // two unrelated prints. One enrolment is one finger, presented twice.
    fingerprint_light(FP_LIGHT_BREATHE, FP_LED_PURPLE, 0);

    // Wait for a finger.
    while (exchange(INS_GET_IMAGE, NULL, 0, NULL, 0, NULL, 300) != CC_OK) {
      if ((int32_t)(now_ms() - deadline) >= 0) goto failed;
      tud_task();
    }
    if (exchange(INS_IMAGE_TO_BUFFER, &pass, 1, NULL, 0, NULL, 1000) != CC_OK) {
      goto failed;
    }
    // A single short flash for "got that one", against three at the end for
    // "done". Same colour, different count: the completion has to be
    // distinguishable from the halfway point without a second hue implying a
    // second finger.
    fingerprint_light(FP_LIGHT_FLASH, FP_LED_GREEN, 1);

    if (pass == 1) {
      // Wait for the lift.
      while (exchange(INS_GET_IMAGE, NULL, 0, NULL, 0, NULL, 300) == CC_OK) {
        if ((int32_t)(now_ms() - deadline) >= 0) goto failed;
        tud_task();
      }
    }
  }

  if (exchange(INS_CREATE_MODEL, NULL, 0, NULL, 0, NULL, 1000) != CC_OK) goto failed;

  uint8_t store[3] = {1, (uint8_t)(slot >> 8), (uint8_t)(slot & 0xff)};
  if (exchange(INS_STORE_TEMPLATE, store, sizeof(store), NULL, 0, NULL, 1000) != CC_OK) {
    goto failed;
  }

  template_count++;
  fingerprint_light(FP_LIGHT_FLASH, FP_LED_GREEN, 3);
  printf("fingerprint: enrolled slot %u\n", slot);
  return true;

failed:
  fingerprint_light(FP_LIGHT_FLASH, FP_LED_RED, 3);
  printf("fingerprint: enrolment failed\n");
  return false;
}

bool fingerprint_erase_all(void) {
  if (!module_present) return false;
  bool ok = exchange(INS_ERASE_ALL, NULL, 0, NULL, 0, NULL, 2000) == CC_OK;
  if (ok) template_count = 0;
  return ok;
}

bool fingerprint_light(fp_light_t effect, fp_color_t color, uint8_t cycles) {
  if (!module_present) return false;
  // Start colour and end colour are sent separately; the same value in both is
  // a plain effect rather than a transition between two colours.
  uint8_t params[4] = {(uint8_t)effect, (uint8_t)color, (uint8_t)color, cycles};
  return exchange(INS_AURA_LED, params, sizeof(params), NULL, 0, NULL, 500) == CC_OK;
}

const char *fingerprint_status_text(void) {
  static char text[16];
  if (!module_present) return "absent";
  snprintf(text, sizeof(text), "%u", template_count);
  return text;
}

uint16_t fingerprint_read_info_page(uint8_t *out, uint16_t cap) {
  if (!module_present || !out) return 0;
  // An acknowledgement first, then the page itself as a data stream.
  uint8_t cc = exchange(INS_READ_INFO_PAGE, NULL, 0, NULL, 0, NULL, 3000);
  if (cc != CC_OK) {
    printf("fingerprint: info page refused, cc=%02x\n", cc);
    return 0;
  }
  return read_data_stream(out, cap, 3000);
}

bool fingerprint_chip_serial(uint8_t out[FP_CHIP_SERIAL_LEN]) {
  if (!module_present || !out) return false;
  // One reserved parameter byte, per the manual's packet layout.
  uint8_t param = 0;
  uint8_t payload[FP_CHIP_SERIAL_LEN];
  uint8_t payload_len = 0;
  if (exchange(INS_CHIP_SERIAL, &param, 1, payload, sizeof(payload), &payload_len,
               1000) != CC_OK || payload_len < FP_CHIP_SERIAL_LEN) {
    return false;
  }
  memcpy(out, payload, FP_CHIP_SERIAL_LEN);
  return true;
}
static uint16_t rd16(const uint8_t *p, uint16_t off) {
  return (uint16_t)((p[off] << 8) | p[off + 1]);
}
static uint32_t rd32(const uint8_t *p, uint16_t off) {
  return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
         ((uint32_t)p[off + 2] << 8) | p[off + 3];
}

bool fingerprint_read_info(fp_info_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));

  uint8_t page[512];
  uint16_t len = fingerprint_read_info_page(page, sizeof(page));
  if (len < 32) {
    printf("fingerprint: info page too short (%u bytes)\n", (unsigned)len);
    return false;
  }

  // Fixed offsets, from the manual's parameter table and confirmed against a
  // real module. This used to scan for a run of printable ASCII, which landed
  // four bytes late: the fields begin at 28, but byte 25 holds a 0x01 that broke
  // the run, so the scan settled on 32 and sliced every string mid-word.
  //
  // The layout is corroborated at several independent points — the device
  // address reads 0xFFFFFFFF at 8, the baud coefficient at 14 gives exactly the
  // rate the module answers on, and the table flag at 126 is the 0x1234 the
  // manual says marks an initialised table.
  out->device_address  = rd32(page, 8);
  out->capacity        = rd16(page, 4);
  out->baud            = (uint32_t)rd16(page, 14) * 9600u;
  out->security_level  = rd16(page, 20);
  out->password        = rd32(page, 60);
  out->table_flag      = rd16(page, 126);

  char *fields[4] = {out->product_model, out->sw_version,
                     out->manufacturer, out->sensor_name};
  for (int f = 0; f < 4; f++) {
    memcpy(fields[f], page + 28 + (f * 8), 8);
    fields[f][8] = '\0';
    for (int n = 8; n > 0; n--) {
      if (fields[f][n - 1] == '\0' || fields[f][n - 1] == ' ') fields[f][n - 1] = '\0';
      else break;
    }
  }
  return true;
}

// Everything the module sends inside a window, framed or not.
//
// read_data_stream() would be the wrong instrument for a probe: it discards
// anything that is not a well-formed PID_DATA/PID_END packet, so "the module
// sent nothing" and "the module sent something I refused to parse" come back
// identically. Here the distinction is the entire result.
static uint16_t drain_raw(uint8_t *out, uint16_t cap, uint32_t window_ms) {
  uint16_t n = 0;
  uint32_t deadline = now_ms() + window_ms;
  while (n < cap) {
    uint8_t byte = 0;
    if (!read_byte(&byte, deadline)) break;
    out[n++] = byte;
  }
  return n;
}

uint8_t fingerprint_security_probe(uint8_t *out, uint16_t cap, uint16_t *out_len,
                                   uint8_t *control_cc) {
  if (out_len) *out_len = 0;
  if (control_cc) *control_cc = 0xff;
  if (!module_present) return 0xff;

  // PS_GetCiphertext, to find out whether this module implements the safety
  // instruction set — the challenge-response that would make the sensor link
  // forgeable only with a key. 0xE2 is the only member that is harmless if a
  // module executes it anyway: it generates a random number and returns it.
  // 0xE0 clears enrolled templates as its first act and 0xE1 cannot be undone.
  uint8_t cc = exchange(INS_SEC_GET_CIPHERTEXT, NULL, 0, NULL, 0, NULL, 1000);

  // 0x00 means "subsequent data packets will be sent" for this command, so
  // whatever follows has to be taken off the line either way: read_ack() rejects
  // any packet that is not an acknowledgement, and one left in the pipe makes
  // the *next* command fail before the link resynchronises on the one after.
  if (cc == CC_OK) {
    uint16_t n = drain_raw(out, cap, 400);
    if (out_len) *out_len = n;
  }

  // The control, and the reason this is worth running at all.
  //
  // An acknowledgement of 0x00 to 0xE2 only means something if this module can
  // say no. Plenty of modules in this family answer every opcode they do not
  // recognise with the same success code, and against one of those the whole
  // safety-instruction lead is an artefact. So ask it something that certainly
  // does not exist: 0x7F is outside every range the manual assigns, and far
  // enough from 0xE0-0xE4 not to land in some adjacent undocumented handler.
  //
  // Different codes mean the module distinguishes opcodes and 0xE2 is real.
  // Identical codes mean it acknowledges anything and 0xE2 proved nothing.
  uint8_t control = exchange(0x7f, NULL, 0, NULL, 0, NULL, 1000);
  if (control == CC_OK) {
    uint8_t scratch[32];
    drain_raw(scratch, sizeof(scratch), 200);
  }
  if (control_cc) *control_cc = control;
  return cc;
}

bool fingerprint_random(uint32_t *out) {
  if (!module_present || !out) return false;
  uint8_t payload[8];
  uint8_t payload_len = 0;
  if (exchange(INS_GET_RANDOM, NULL, 0, payload, sizeof(payload), &payload_len,
               500) != CC_OK || payload_len < 4) {
    return false;
  }
  *out = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
         ((uint32_t)payload[2] << 8) | payload[3];
  return true;
}

#else  // no sensor configured for this board

void fingerprint_init(void) {}
bool fingerprint_present(void) { return false; }
uint16_t fingerprint_template_count(void) { return 0; }
bool fingerprint_finger_down(void) { return false; }
bool fingerprint_verify(uint16_t *slot, uint16_t *score) { (void)slot; (void)score; return false; }
bool fingerprint_enroll(uint16_t slot, uint32_t timeout_ms) { (void)slot; (void)timeout_ms; return false; }
bool fingerprint_erase_all(void) { return false; }
bool fingerprint_light(fp_light_t e, fp_color_t c, uint8_t n) { (void)e; (void)c; (void)n; return false; }
bool fingerprint_random(uint32_t *out) { (void)out; return false; }
uint8_t fingerprint_security_probe(uint8_t *o, uint16_t c, uint16_t *l, uint8_t *k) {
  (void)o; (void)c; if (l) *l = 0; if (k) *k = 0xff; return 0xff;
}
bool fingerprint_read_info(fp_info_t *out) { (void)out; return false; }
bool fingerprint_chip_serial(uint8_t out[FP_CHIP_SERIAL_LEN]) { (void)out; return false; }
uint16_t fingerprint_read_info_page(uint8_t *o, uint16_t c) { (void)o; (void)c; return 0; }
const char *fingerprint_status_text(void) { return "absent"; }
bool fingerprint_touch_wired(void) { return false; }
bool fingerprint_touch_asserted(void) { return false; }
bool fingerprint_probe(void) { return false; }
bool fingerprint_auto_enroll(uint16_t s, uint8_t e, uint32_t t) {
  (void)s; (void)e; (void)t; return false;
}
uint8_t fingerprint_last_enroll_cc(void) { return 0; }
uint32_t fingerprint_probe_baud(void) { return 0; }
uint16_t fingerprint_probe_rx_bytes(void) { return 0; }
uint16_t fingerprint_boot_rx_bytes(void) { return 0; }
bool fingerprint_boot_saw_hello(void) { return false; }
uint16_t fingerprint_line_high(bool rx_pin) { (void)rx_pin; return 0; }
uint16_t fingerprint_line_edges(bool rx_pin) { (void)rx_pin; return 0; }
uint32_t fingerprint_line_min_pulse_us(void) { return 0; }
bool fingerprint_probe_saw_hello(void) { return false; }

#endif
