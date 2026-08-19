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
#define INS_FAST_SEARCH     0x1b
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
static uint16_t template_count;

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
  uart_init(FP_UART, FINGERPRINT_BAUD);
  gpio_set_function(FINGERPRINT_UART_TX, GPIO_FUNC_UART);
  gpio_set_function(FINGERPRINT_UART_RX, GPIO_FUNC_UART);
  uart_set_format(FP_UART, 8, 1, UART_PARITY_NONE);
  uart_set_fifo_enabled(FP_UART, true);

  // The module takes a moment after power-up before it will answer.
  sleep_ms(200);

  // Handshake with the default all-zero password. Two attempts: the first
  // often lands while the module is still starting.
  uint8_t password[4] = {0, 0, 0, 0};
  for (int attempt = 0; attempt < 2 && !module_present; attempt++) {
    module_present = exchange(INS_VERIFY_PASSWORD, password, sizeof(password),
                              NULL, 0, NULL, 500) == CC_OK;
  }

  if (!module_present) {
    printf("fingerprint: no module on uart (button remains the trigger)\n");
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

uint16_t fingerprint_template_count(void) {
  return template_count;
}

bool fingerprint_finger_down(void) {
  if (!module_present) return false;
  return exchange(INS_GET_IMAGE, NULL, 0, NULL, 0, NULL, 300) == CC_OK;
}

bool fingerprint_verify(uint16_t *slot, uint16_t *score) {
  if (!module_present || template_count == 0) return false;

  // GET IMAGE must succeed first; it is also how "no finger" is reported.
  uint8_t cc = exchange(INS_GET_IMAGE, NULL, 0, NULL, 0, NULL, 500);
  if (cc != CC_OK) return false;

  uint8_t buffer_index = 1;
  if (exchange(INS_IMAGE_TO_BUFFER, &buffer_index, 1, NULL, 0, NULL, 1000) != CC_OK) {
    // A smudged or partial press lands here. Not an error worth reporting: the
    // user simply presses again.
    return false;
  }

  // Search the whole bank: buffer, start page (high/low), count (high/low).
  uint8_t params[5] = {1, 0x00, 0x00, (uint8_t)(0xff), (uint8_t)(0xff)};
  uint8_t payload[8];
  uint8_t payload_len = 0;
  cc = exchange(INS_FAST_SEARCH, params, sizeof(params), payload, sizeof(payload),
                &payload_len, 2000);
  if (cc != CC_OK || payload_len < 4) return false;

  if (slot) *slot = ((uint16_t)payload[0] << 8) | payload[1];
  if (score) *score = ((uint16_t)payload[2] << 8) | payload[3];
  return true;
}

bool fingerprint_enroll(uint16_t slot, uint32_t timeout_ms) {
  if (!module_present) return false;
  uint32_t deadline = now_ms() + timeout_ms;

  // Two impressions of the same finger, with a lift in between. The lift is not
  // politeness: without it the module merges two captures of an identical
  // placement and builds a template that only matches that exact position.
  for (uint8_t pass = 1; pass <= 2; pass++) {
    fingerprint_light(FP_LIGHT_BREATHE, pass == 1 ? FP_LED_BLUE : FP_LED_PURPLE, 0);

    // Wait for a finger.
    while (exchange(INS_GET_IMAGE, NULL, 0, NULL, 0, NULL, 300) != CC_OK) {
      if ((int32_t)(now_ms() - deadline) >= 0) goto failed;
      tud_task();
    }
    if (exchange(INS_IMAGE_TO_BUFFER, &pass, 1, NULL, 0, NULL, 1000) != CC_OK) {
      goto failed;
    }
    fingerprint_light(FP_LIGHT_FLASH, FP_LED_GREEN, 2);

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

bool fingerprint_read_info(fp_info_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));

  uint8_t page[512];
  uint16_t len = fingerprint_read_info_page(page, sizeof(page));
  if (len < 32) {
    printf("fingerprint: info page too short (%u bytes)\n", (unsigned)len);
    return false;
  }

  // The page layout is not documented anywhere we could find — Hi-Link's
  // datasheet defers the command set to a protocol note that does not appear to
  // be published at all. So this scans for the first run of four consecutive
  // 8-byte printable-ASCII fields, which is what the one open-source driver for
  // these parts does; evidently its author had no layout either.
  //
  // The offset is therefore discovered, not known. First contact with a real
  // module should dump the whole page and confirm this lands on the intended
  // four fields rather than on some other ASCII run that happens to come first.
  uint16_t limit = (uint16_t)((len > 128 ? 128 : len) - 32);
  for (uint16_t off = 0; off <= limit; off += 8) {
    bool run_ok = true;
    for (uint16_t f = 0; f < 32 && run_ok; f++) {
      uint8_t c = page[off + f];
      if (c != 0x00 && (c < 0x20 || c > 0x7e)) run_ok = false;
    }
    if (!run_ok) continue;

    char *fields[4] = {out->product_sn, out->sw_version,
                       out->manufacturer, out->sensor_name};
    for (int f = 0; f < 4; f++) {
      memcpy(fields[f], page + off + (f * 8), 8);
      fields[f][8] = '\0';
      for (int n = 8; n > 0; n--) {
        if (fields[f][n - 1] == '\0' || fields[f][n - 1] == ' ') fields[f][n - 1] = '\0';
        else break;
      }
    }
    return true;
  }

  printf("fingerprint: info page held no recognisable field run\n");
  return false;
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
bool fingerprint_read_info(fp_info_t *out) { (void)out; return false; }
uint16_t fingerprint_read_info_page(uint8_t *o, uint16_t c) { (void)o; (void)c; return 0; }
const char *fingerprint_status_text(void) { return "absent"; }

#endif
