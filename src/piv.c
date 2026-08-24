#include "piv.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha256.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "board_config.h"
#include "settings.h"
#include "storage.h"
#include "trace.h"

#define LOG(...) do { printf("piv: "); printf(__VA_ARGS__); printf("\n"); } while (0)

// Compile-time identity. main/CMakeLists.txt seeds this from secrets.example.h
// when it is missing, so the include is unconditional and the compiler tracks
// it as a dependency. Placeholder content is detected below and ignored, which
// is what makes the file effectively optional.

static const uint8_t PIV_AID[] = {0xa0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10, 0x00};
static const uint8_t PIV_AID_VERSIONED[] = {0xa0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00};
// Private AID: "F0" marks a proprietary registered identifier per ISO 7816-5,
// followed by "SMCARD". Only our CryptoTokenKit driver claims this, which is
// the point — see PIV_ANSWER_STANDARD_AID in board_config.h.
static const uint8_t PRIVATE_AID[] = {0xf0, 0x53, 0x4d, 0x43, 0x41, 0x52, 0x44};
static const uint8_t DISCOVERY_OBJECT[] = {
  0x7e, 0x12,
  0x4f, 0x0b, 0xa0, 0x00, 0x00, 0x03, 0x08, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00,
  0x5f, 0x2f, 0x02, 0x60, 0x00
};
static const uint8_t CCC_OBJECT[] = {
  0x53, 0x24,
  0xf0, 0x15, 0xa0, 0x00, 0x00, 0x01, 0x16, 0xff, 0x02, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
  0xf1, 0x01, 0x21,
  0xf2, 0x01, 0x21,
  0xf3, 0x00,
  0xf4, 0x01, 0x00,
  0xf5, 0x01, 0x10
};
static uint8_t CHUID_OBJECT[] = {
  0x53, 0x3b,
  0x30, 0x19, 0xd4, 0xe7, 0x39, 0xda, 0x73, 0x9c, 0xed, 0x39, 0xce, 0x73,
              0x9d, 0x83, 0x68, 0x58, 0x21, 0x08, 0x42, 0x10, 0x84, 0x21,
              0xc8, 0x42, 0x10, 0xc3, 0xeb,
  0x34, 0x10, 0x01, 0x30, 0x19, 0xd4, 0xe7, 0x39, 0xda, 0x73, 0x9c, 0xed,
              0x39, 0xce, 0x73, 0x9d, 0x83, 0x68,
  0x35, 0x08, 0x32, 0x30, 0x33, 0x36, 0x30, 0x37, 0x30, 0x33,
  0x3e, 0x00,
  0xfe, 0x00
};
#define CHUID_GUID_OFFSET 31
static const uint8_t KEY_HISTORY_OBJECT[] = {
  0x53, 0x09,
  0xc1, 0x01, 0x00,
  0xc2, 0x01, 0x00,
  0xc3, 0x01, 0x00
};

static mbedtls_pk_context auth_key;
static mbedtls_pk_context key_mgmt_key;
static bool piv_keys_initialized;
static uint8_t cert_9a_der[1536];
static size_t cert_9a_der_len;
static uint8_t cert_9d_der[1536];
static size_t cert_9d_der_len;
static const char *key_source = "none";
// Last mbedtls_pk_parse_key() result, surfaced in STATUS. Without it a parse
// failure is invisible: the certificate still decodes, so the card enumerates
// and only signing fails.
static int key_parse_rc;
static uint8_t pending_response[1800];
static size_t pending_response_len;
static size_t pending_response_off;
// Scratch space for the certificate objects assembled by handle_get_data.
// Static rather than automatic: APDUs are dispatched from the TinyUSB task,
// and 1.7 kB of stack there is more than that task can spare.
static uint8_t object_scratch[1700];
static uint8_t chained_apdu_data[700];
static size_t chained_apdu_data_len;
static uint8_t chained_ins;
static uint8_t chained_p1;
static uint8_t chained_p2;
static uint32_t pin_verified_until;
static uint32_t user_presence_until;
// Same press, second window. See the key agreement branch for why it cannot
// share user_presence_until.
static uint32_t session_presence_until;

// True when the device cannot authenticate for anybody yet, because a sensor is
// fitted and nothing is enrolled on it.
//
// Worth a status word of its own. "No presence yet" and "this device is not set
// up" are both refusals, but only one of them can be fixed by touching the
// sensor, and a host that cannot tell them apart can only offer the wrong
// advice. 6982 means authentication is needed; 6985 — conditions of use not
// satisfied — means the request cannot succeed in the device's current state,
// which is exactly the distinction.
static bool setup_incomplete;

// True when the fingerprint module is not the one this device was set up with.
// Everything is refused: the sensor is what stands between a stolen device and
// its key, so a swapped one is the cheapest way past it and must not be a way
// past it at all.
static bool module_mismatch;
void piv_set_module_mismatch(bool mismatch) { module_mismatch = mismatch; }
bool piv_module_mismatch(void) { return module_mismatch; }
void piv_set_setup_incomplete(bool incomplete) { setup_incomplete = incomplete; }
static uint32_t challenge_until;
static uint32_t signature_until;

static const uint32_t PIN_VERIFIED_WINDOW_MS = (60000);
static const uint32_t USER_PRESENCE_WINDOW_MS = (10000);
// Slot 9d's gate. Deliberately longer than the 9a presence window and never
// consumed: one press has to cover the login and the keychain work that follows
// it, which arrives in several separate operations over some seconds.
static const uint32_t SESSION_PRESENCE_WINDOW_MS = (60000);
static const uint32_t CHALLENGE_WINDOW_MS = (15000);
static const uint32_t SIGNATURE_ACK_MS = (700);

static size_t encode_len(uint8_t *out, size_t len);
static bool respond_data(const uint8_t *data, size_t data_len, uint8_t *response,
                         size_t *response_len, size_t response_cap);
static void note_challenge(void);

static void decode_pem_cert(const char *pem, uint8_t *der, size_t der_cap, size_t *der_len) {
  *der_len = 0;
  const char *begin = strstr(pem, "-----BEGIN CERTIFICATE-----");
  const char *end = strstr(pem, "-----END CERTIFICATE-----");
  if (!begin || !end || end <= begin) return;
  begin = strchr(begin, '\n');
  if (!begin) return;
  begin++;
  size_t b64_len = (size_t)(end - begin);
  int rc = mbedtls_base64_decode(der, der_cap, der_len,
                                 (const unsigned char *)begin, b64_len);
  if (rc != 0) {
    *der_len = 0;
    LOG("certificate DER decode failed: -0x%x", -rc);
  }
}

const char *piv_key_source_name(void) {
  return key_source;
}

int piv_key_parse_error(void) {
  return key_parse_rc;
}

const char *piv_algorithm_name(void) {
  switch (mbedtls_pk_get_type(&auth_key)) {
    case MBEDTLS_PK_RSA: return "rsa2048";
    case MBEDTLS_PK_ECKEY: return "p256";
    default: return "none";
  }
}

bool piv_has_identity(void) {
  return cert_9a_der_len != 0;
}

static bool append_sw(uint8_t *response, size_t *response_len, size_t response_cap,
                      uint16_t sw) {
  if (*response_len + 2 > response_cap) return false;
  response[(*response_len)++] = (uint8_t)(sw >> 8);
  response[(*response_len)++] = (uint8_t)(sw & 0xff);
  return true;
}

static bool respond_data(const uint8_t *data, size_t data_len, uint8_t *response,
                         size_t *response_len, size_t response_cap) {
  if (data_len + 2 > response_cap) return false;
  memcpy(response, data, data_len);
  *response_len = data_len;
  return append_sw(response, response_len, response_cap, 0x9000);
}

static size_t encode_len(uint8_t *out, size_t len) {
  if (len < 0x80) {
    out[0] = (uint8_t)len;
    return 1;
  }
  if (len <= 0xff) {
    out[0] = 0x81;
    out[1] = (uint8_t)len;
    return 2;
  }
  out[0] = 0x82;
  out[1] = (uint8_t)(len >> 8);
  out[2] = (uint8_t)len;
  return 3;
}

static size_t encoded_len_size(size_t len) {
  if (len < 0x80) return 1;
  if (len <= 0xff) return 2;
  return 3;
}

static size_t apdu_le(const uint8_t *apdu, size_t apdu_len, size_t default_len) {
  if (apdu_len == 4) return default_len;
  if (apdu_len == 5) return apdu[4] == 0 ? 256 : apdu[4];
  uint8_t lc = apdu[4];
  if (apdu_len > 5 + lc) return apdu[5 + lc] == 0 ? 256 : apdu[5 + lc];
  return default_len;
}

static bool respond_maybe_chunked(const uint8_t *data, size_t data_len,
                                  const uint8_t *apdu, size_t apdu_len,
                                  uint8_t *response, size_t *response_len,
                                  size_t response_cap) {
  size_t le = apdu_le(apdu, apdu_len, response_cap - 2);
  if (le > response_cap - 2) le = response_cap - 2;
  if (le >= data_len) return respond_data(data, data_len, response, response_len, response_cap);
  // Keep the CCID bulk transfer off an exact multiple of the 64-byte packet
  // size so the host is not left waiting for a zero-length packet.
  if (((le + 12) % 64) == 0 && le > 1) le--;

  if (data_len > sizeof(pending_response)) return false;
  memcpy(pending_response, data, data_len);
  pending_response_len = data_len;
  pending_response_off = le;
  memcpy(response, data, le);
  *response_len = le;
  size_t remain = pending_response_len - pending_response_off;
  uint16_t sw = (uint16_t)(0x6100 | (remain > 255 ? 0x00 : remain));
  return append_sw(response, response_len, response_cap, sw);
}

static bool handle_get_response(const uint8_t *apdu, size_t apdu_len,
                                uint8_t *response, size_t *response_len,
                                size_t response_cap) {
  if (pending_response_off >= pending_response_len) {
    pending_response_len = 0;
    pending_response_off = 0;
    return append_sw(response, response_len, response_cap, 0x6a86);
  }
  size_t le = apdu_le(apdu, apdu_len, response_cap - 2);
  size_t remain = pending_response_len - pending_response_off;
  size_t take = remain < le ? remain : le;
  if (take > response_cap - 2) take = response_cap - 2;
  if (((take + 12) % 64) == 0 && take > 1) take--;
  memcpy(response, pending_response + pending_response_off, take);
  pending_response_off += take;
  *response_len = take;
  remain = pending_response_len - pending_response_off;
  if (remain == 0) {
    pending_response_len = 0;
    pending_response_off = 0;
    return append_sw(response, response_len, response_cap, 0x9000);
  }
  uint16_t sw = (uint16_t)(0x6100 | (remain > 255 ? 0x00 : remain));
  return append_sw(response, response_len, response_cap, sw);
}

static bool read_lc_data(const uint8_t *apdu, size_t apdu_len,
                         const uint8_t **data, size_t *data_len) {
  if (apdu_len < 5) return false;
  if (apdu[4] == 0x00) {
    if (apdu_len < 7) return false;
    size_t lc = ((size_t)apdu[5] << 8) | apdu[6];
    if (apdu_len < 7 + lc) return false;
    *data = apdu + 7;
    *data_len = lc;
    return true;
  }
  uint8_t lc = apdu[4];
  if (apdu_len < 5 + lc) return false;
  *data = apdu + 5;
  *data_len = lc;
  return true;
}

static bool tlv_read_len(const uint8_t *buf, size_t buf_len, size_t *off, size_t *len) {
  if (*off >= buf_len) return false;
  uint8_t b = buf[(*off)++];
  if ((b & 0x80) == 0) {
    *len = b;
    return true;
  }
  size_t n = b & 0x7f;
  if (n == 0 || n > 2 || *off + n > buf_len) return false;
  size_t v = 0;
  for (size_t i = 0; i < n; i++) v = (v << 8) | buf[(*off)++];
  *len = v;
  return true;
}

static bool tlv_find_one(const uint8_t *buf, size_t buf_len, uint8_t tag,
                         const uint8_t **value, size_t *value_len) {
  size_t off = 0;
  while (off < buf_len) {
    uint8_t t = buf[off++];
    size_t len = 0;
    if (!tlv_read_len(buf, buf_len, &off, &len) || off + len > buf_len) return false;
    if (t == tag) {
      *value = buf + off;
      *value_len = len;
      return true;
    }
    off += len;
  }
  return false;
}

// Not in the nonce path, and must not become so. Signing is RFC 6979
// deterministic (MBEDTLS_ECDSA_DETERMINISTIC), which derives the nonce from the
// key and the message rather than from here — a predictable nonce recovers the
// private key from a single signature, so do not turn that off. This feeds
// blinding and padding, where the RP2350's TRNG is more than sufficient.
static int piv_rng(void *ctx, unsigned char *out, size_t len) {
  (void)ctx;
  while (len >= sizeof(uint64_t)) {
    uint64_t value = get_rand_64();
    memcpy(out, &value, sizeof(value));
    out += sizeof(value);
    len -= sizeof(value);
  }
  if (len) {
    uint64_t value = get_rand_64();
    memcpy(out, &value, len);
  }
  return 0;
}

static uint32_t now_ms(void) {
  return to_ms_since_boot(get_absolute_time());
}

// Set once our driver claims the card, and once the host first says anything
// at all. Together they answer "is anybody there, and is it us?" — which is
// what the revert in main.c turns on.
static bool private_aid_selected;
static uint32_t first_contact_ms;
static uint32_t upgrade_requested_ms;

bool piv_private_aid_selected(void) {
  return private_aid_selected;
}

uint32_t piv_first_contact_ms(void) {
  return first_contact_ms;
}

uint32_t piv_upgrade_requested_ms(void) {
  return upgrade_requested_ms;
}

static bool handle_select(const uint8_t *apdu, size_t apdu_len, uint8_t *response,
                          size_t *response_len, size_t response_cap) {
  const uint8_t *data = NULL;
  size_t data_len = 0;
  if (!read_lc_data(apdu, apdu_len, &data, &data_len)) {
    return append_sw(response, response_len, response_cap, 0x6700);
  }
  // Pinpad mode answers only the private AID, which hides the card from Apple's
  // pivtoken so our driver binds deterministically.
  //
  // Standard mode answers the PIV AID *and* the private one — the latter purely
  // as a probe. Nothing else on this system knows to ask for our private AID, so
  // a select of it is proof that our driver is installed, and the device
  // upgrades itself rather than needing an installer to tell it.
  //
  // Answering both is only safe because it does not last. A card that stays in
  // that state leaves two drivers in the running, and macOS binds exactly one
  // per card at insertion — a race we measured flipping between reboots, which
  // also left sc_auth filing one identity under "not used for authentication"
  // while that card silently stopped authenticating. Here the state resolves
  // within a few hundred milliseconds: the probe is answered, the mode is
  // persisted, and the device reboots into exclusive pinpad mode.
  bool pinpad_mode = settings_aid_mode() == AID_MODE_PINPAD;

  bool private_aid = data_len == sizeof(PRIVATE_AID) &&
                     memcmp(data, PRIVATE_AID, sizeof(PRIVATE_AID)) == 0;
  bool base_aid = !pinpad_mode && data_len == sizeof(PIV_AID) &&
                  memcmp(data, PIV_AID, sizeof(PIV_AID)) == 0;
  bool versioned_aid = !pinpad_mode && data_len == sizeof(PIV_AID_VERSIONED) &&
                       memcmp(data, PIV_AID_VERSIONED, sizeof(PIV_AID_VERSIONED)) == 0;

  if (!base_aid && !versioned_aid && !private_aid) {
    return append_sw(response, response_len, response_cap, 0x6a82);
  }
  // A successful select of the private AID is proof our driver is installed and
  // bound: nothing else on the system knows to ask for it.
  if (private_aid) {
    private_aid_selected = true;
    // Answered from standard mode, this was the probe. Note the moment; main.c
    // finishes the response, then switches modes and reboots.
    if (!pinpad_mode && upgrade_requested_ms == 0) upgrade_requested_ms = now_ms();
  }
  const uint8_t fci[] = {
    0x61, 0x11,
    0x4f, 0x06, 0x00, 0x00, 0x10, 0x00, 0x01, 0x00,
    0x79, 0x07,
    0x4f, 0x05, 0xa0, 0x00, 0x00, 0x03, 0x08
  };
  return respond_data(fci, sizeof(fci), response, response_len, response_cap);
}

// Wraps a certificate DER blob in the 0x53 data object PIV clients expect.
static bool respond_certificate(const uint8_t *der, size_t der_len,
                                const uint8_t *apdu, size_t apdu_len,
                                uint8_t *response, size_t *response_len,
                                size_t response_cap) {
  if (der_len == 0) return append_sw(response, response_len, response_cap, 0x6a88);
  size_t inner_len = 1 + encoded_len_size(der_len) + der_len + 3 + 2;
  if (1 + encoded_len_size(inner_len) + inner_len > sizeof(object_scratch)) {
    return append_sw(response, response_len, response_cap, 0x6f00);
  }

  size_t off = 0;
  object_scratch[off++] = 0x53;
  off += encode_len(object_scratch + off, inner_len);
  object_scratch[off++] = 0x70;
  off += encode_len(object_scratch + off, der_len);
  memcpy(object_scratch + off, der, der_len);
  off += der_len;
  object_scratch[off++] = 0x71;
  object_scratch[off++] = 0x01;
  object_scratch[off++] = 0x00;
  object_scratch[off++] = 0xfe;
  object_scratch[off++] = 0x00;
  return respond_maybe_chunked(object_scratch, off, apdu, apdu_len,
                               response, response_len, response_cap);
}

static bool handle_get_data(const uint8_t *apdu, size_t apdu_len, uint8_t *response,
                            size_t *response_len, size_t response_cap) {
  const uint8_t *data = NULL;
  size_t data_len = 0;
  if (!read_lc_data(apdu, apdu_len, &data, &data_len)) {
    return append_sw(response, response_len, response_cap, 0x6700);
  }

  if (data_len == 3 && data[0] == 0x5c && data[1] == 0x01 && data[2] == 0x7e) {
    return respond_maybe_chunked(DISCOVERY_OBJECT, sizeof(DISCOVERY_OBJECT), apdu, apdu_len,
                                 response, response_len, response_cap);
  }

  if (data_len != 5 || data[0] != 0x5c || data[1] != 0x03 ||
      data[2] != 0x5f || data[3] != 0xc1) {
    return append_sw(response, response_len, response_cap, 0x6a88);
  }

  switch (data[4]) {
    case 0x07:  // card capability container
      return respond_maybe_chunked(CCC_OBJECT, sizeof(CCC_OBJECT), apdu, apdu_len,
                                   response, response_len, response_cap);
    case 0x02:  // card holder unique identifier
      return respond_maybe_chunked(CHUID_OBJECT, sizeof(CHUID_OBJECT), apdu, apdu_len,
                                   response, response_len, response_cap);
    case 0x05:  // slot 9a certificate (authentication)
      return respond_certificate(cert_9a_der, cert_9a_der_len, apdu, apdu_len,
                                 response, response_len, response_cap);
    case 0x0b:  // slot 9d certificate (key management)
      return respond_certificate(cert_9d_der, cert_9d_der_len, apdu, apdu_len,
                                 response, response_len, response_cap);
    case 0x0c:  // key history
      return respond_maybe_chunked(KEY_HISTORY_OBJECT, sizeof(KEY_HISTORY_OBJECT), apdu, apdu_len,
                                   response, response_len, response_cap);
    default:
      return append_sw(response, response_len, response_cap, 0x6a88);
  }
}

// The PIN is deliberately not checked. macOS requires a PIN prompt before it
// will use a PIV key, but on this device authorization comes from the button
// press that gates slot 9a below, not from the digits typed into that prompt.
static bool handle_verify(const uint8_t *apdu, size_t apdu_len,
                          uint8_t *response, size_t *response_len, size_t response_cap) {
  const uint8_t *data = NULL;
  size_t data_len = 0;
  if (apdu[2] != 0x00 || apdu[3] != 0x80 ||
      !read_lc_data(apdu, apdu_len, &data, &data_len)) {
    pin_verified_until = 0;
    return append_sw(response, response_len, response_cap, 0x6a86);
  }
  (void)data;
  (void)data_len;
  pin_verified_until = now_ms() + PIN_VERIFIED_WINDOW_MS;
  return append_sw(response, response_len, response_cap, 0x9000);
}

static bool window_open(uint32_t deadline, uint32_t window) {
  return deadline != 0 && (uint32_t)(deadline - now_ms()) <= window;
}

void piv_note_pin_verified(void) {
  pin_verified_until = now_ms() + PIN_VERIFIED_WINDOW_MS;
}

// What proved presence, for the trace. On a production unit this is always the
// fingerprint; the label said BUTTON regardless, which is actively misleading in
// exactly the traces used to debug the sensor.
static const char *piv_presence_source = "PRESENCE";
void piv_set_presence_source(const char *source) { piv_presence_source = source; }

void piv_note_user_presence(void) {
  user_presence_until = now_ms() + USER_PRESENCE_WINDOW_MS;
  session_presence_until = now_ms() + SESSION_PRESENCE_WINDOW_MS;
  // The press answered whatever macOS was asking, so stop asking for it.
  challenge_until = 0;
  trace_event(piv_presence_source);
  LOG("user presence accepted");
}

// Asks the status LED to request a press.
//
// Only the refused-signature path calls this. SELECT and VERIFY look like they
// should too, but the trace in README.md shows macOS sends the card nothing at
// all until a PIN has been submitted — so both of them only ever arrive after
// the user has already pressed, and lighting the LED there just blinks at
// someone who is finished. Anticipating a prompt is the host agent's job.
static void note_challenge(void) {
  challenge_until = now_ms() + CHALLENGE_WINDOW_MS;
}

const void *piv_auth_key(void) {
  return mbedtls_pk_get_type(&auth_key) == MBEDTLS_PK_NONE ? NULL : &auth_key;
}

bool piv_challenge_active(void) {
  return window_open(challenge_until, CHALLENGE_WINDOW_MS);
}

bool piv_recent_signature(void) {
  return window_open(signature_until, SIGNATURE_ACK_MS);
}

// Wraps a payload in the dynamic authentication template PIV answers with:
// 7C L { 82 L <payload> }. Both a signature and an ECDH shared secret come back
// this way, so the two paths share it rather than each building their own.
static bool respond_dynamic_auth(const uint8_t *payload, size_t payload_len,
                                 uint8_t *response, size_t *response_len,
                                 size_t response_cap) {
  size_t off = 0;
  response[off++] = 0x7c;
  off += encode_len(response + off, 1 + encoded_len_size(payload_len) + payload_len);
  response[off++] = 0x82;
  off += encode_len(response + off, payload_len);
  if (off + payload_len + 2 > response_cap) return false;
  memcpy(response + off, payload, payload_len);
  off += payload_len;
  *response_len = off;
  return append_sw(response, response_len, response_cap, 0x9000);
}

static bool handle_general_authenticate(const uint8_t *apdu, size_t apdu_len,
                                        uint8_t *response, size_t *response_len,
                                        size_t response_cap) {
  if (!(apdu[3] == 0x9a || apdu[3] == 0x9d)) {
    return append_sw(response, response_len, response_cap, 0x6a86);
  }
  if (module_mismatch) {
    // 6983, authentication method blocked: the way in exists and has been shut,
    // which is a different thing from needing a step or being unconfigured.
    LOG("refused: fingerprint module is not the one this device was bound to");
    return append_sw(response, response_len, response_cap, 0x6983);
  }

  if (setup_incomplete) {
    LOG("refused: no fingerprint enrolled, the device is not set up");
    return append_sw(response, response_len, response_cap, 0x6985);
  }

  if (!window_open(pin_verified_until, PIN_VERIFIED_WINDOW_MS)) {
    pin_verified_until = 0;
    return append_sw(response, response_len, response_cap, 0x6982);
  }
  const uint8_t *data = NULL;
  size_t data_len = 0;
  if (!read_lc_data(apdu, apdu_len, &data, &data_len)) {
    return append_sw(response, response_len, response_cap, 0x6700);
  }

  size_t outer_off = 0;
  if (data_len < 2 || data[outer_off++] != 0x7c) {
    return append_sw(response, response_len, response_cap, 0x6a80);
  }
  size_t outer_len = 0;
  if (!tlv_read_len(data, data_len, &outer_off, &outer_len) || outer_off + outer_len > data_len) {
    return append_sw(response, response_len, response_cap, 0x6a80);
  }

  // Two different operations share this command. Tag 0x81 carries a challenge
  // to sign; tag 0x85 carries the other party's public point for key agreement,
  // which is what macOS uses to wrap the login keychain unlock key to slot 9D.
  const uint8_t *challenge = NULL;
  size_t challenge_len = 0;
  const uint8_t *peer_point = NULL;
  size_t peer_point_len = 0;
  bool key_agreement =
      tlv_find_one(data + outer_off, outer_len, 0x85, &peer_point, &peer_point_len);

  if (!key_agreement &&
      !tlv_find_one(data + outer_off, outer_len, 0x81, &challenge, &challenge_len)) {
    return append_sw(response, response_len, response_cap, 0x6a80);
  }
  mbedtls_pk_context *key = apdu[3] == 0x9d ? &key_mgmt_key : &auth_key;
  mbedtls_pk_type_t key_type = mbedtls_pk_get_type(key);

  // P1 is the PIV algorithm identifier: 0x07 RSA-2048, 0x11 ECC P-256. It has
  // to match the key actually provisioned, or the host gets a signature it
  // cannot verify.
  uint8_t algorithm = apdu[2];
  bool algorithm_matches = (key_type == MBEDTLS_PK_RSA && algorithm == 0x07) ||
                           (key_type == MBEDTLS_PK_ECKEY && algorithm == 0x11);
  if (!algorithm_matches) {
    pin_verified_until = 0;
    LOG("slot %02x refused: algorithm %02x does not match the loaded key",
        apdu[3], algorithm);
    return append_sw(response, response_len, response_cap, 0x6a86);
  }

  if (key_agreement) {
    // ECDH on P-256: the shared secret is the X coordinate of peer * d.
    //
    // Gated on a press, but against a *separate* window from slot 9a's. Slot 9a
    // consumes its presence on use, and macOS opens the login keychain here
    // immediately after the 9a signature that logged the user in — so checking
    // the same variable would refuse the unwrap that always follows a successful
    // login. That is exactly the 6982 this branch was ungated to fix.
    //
    // So: longer window, never consumed. One press covers the login and the
    // keychain work after it. What it stops is a host using the key management
    // key with no interaction whatsoever, which was previously free — and free
    // to a compromised host, which is the attacker this device exists to resist.
    if (!window_open(session_presence_until, SESSION_PRESENCE_WINDOW_MS)) {
      LOG("slot 9d refused: no fingerprint within the session window");
      // Tell the indicator and the driver that a press would help. Note that the
      // PIN verification is deliberately left standing: consuming it here would
      // make the retry fail for a second, unrelated reason.
      note_challenge();
      return append_sw(response, response_len, response_cap, 0x6982);
    }
    if (key_type != MBEDTLS_PK_ECKEY) {
      return append_sw(response, response_len, response_cap, 0x6a86);
    }

    mbedtls_ecp_group group;
    mbedtls_mpi private_scalar, shared;
    mbedtls_ecp_point public_point, peer;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&private_scalar);
    mbedtls_mpi_init(&shared);
    mbedtls_ecp_point_init(&public_point);
    mbedtls_ecp_point_init(&peer);

    uint8_t secret[32];
    int rc = mbedtls_ecp_export(mbedtls_pk_ec(*key), &group, &private_scalar, &public_point);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&group, &peer, peer_point, peer_point_len);
    if (rc == 0) {
      rc = mbedtls_ecdh_compute_shared(&group, &shared, &peer, &private_scalar,
                                       piv_rng, NULL);
    }
    if (rc == 0) rc = mbedtls_mpi_write_binary(&shared, secret, sizeof(secret));

    mbedtls_ecp_point_free(&peer);
    mbedtls_ecp_point_free(&public_point);
    mbedtls_mpi_free(&shared);
    mbedtls_mpi_free(&private_scalar);
    mbedtls_ecp_group_free(&group);

    if (rc != 0) {
      LOG("slot %02x key agreement failed: -0x%04x", apdu[3], (unsigned)(-rc));
      return append_sw(response, response_len, response_cap, 0x6f00);
    }
    LOG("slot %02x key agreement ok", apdu[3]);
    return respond_dynamic_auth(secret, sizeof(secret), response, response_len,
                                response_cap);
  }

  // Slot 9a is what macOS authenticates logins and sudo with, so each use of it
  // costs one button press. Slot 9d is deliberately not gated the same way —
  // see the key agreement branch above.
  if (apdu[3] == 0x9a) {
    if (!window_open(user_presence_until, USER_PRESENCE_WINDOW_MS)) {
      LOG("slot 9a refused: no fingerprint within the presence window");
      note_challenge();
      pin_verified_until = 0;
      user_presence_until = 0;
      return append_sw(response, response_len, response_cap, 0x6982);
    }
    user_presence_until = 0;
  }

  // RSA-2048 signatures are 256 bytes; a P-256 ECDSA signature is DER and comes
  // out under 80.
  uint8_t sig[256];
  size_t sig_len = sizeof(sig);
  int rc = 0;

  if (key_type == MBEDTLS_PK_RSA && challenge_len == sizeof(sig)) {
    // Already a padded 2048-bit block: apply the raw private key operation.
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*key);
    rc = mbedtls_rsa_private(rsa, piv_rng, NULL, challenge, sig);
  } else if (challenge_len == 32) {
    // Already a SHA-256 digest, which is what a host sends for P-256. Hashing
    // it again would sign the wrong thing.
    rc = mbedtls_pk_sign(key, MBEDTLS_MD_SHA256, challenge, challenge_len,
                         sig, sizeof(sig), &sig_len, piv_rng, NULL);
  } else {
    uint8_t hash[32];
    mbedtls_sha256(challenge, challenge_len, hash, 0);
    rc = mbedtls_pk_sign(key, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                         sig, sizeof(sig), &sig_len, piv_rng, NULL);
  }
  // The PIN verification is deliberately *not* consumed here.
  //
  // It used to be, as belt-and-braces on top of the per-signature button press.
  // That broke the login keychain: macOS signs with 9A to authenticate and then
  // immediately asks 9D to unwrap the keychain unlock key, and the second
  // request arrived to a card that had just forgotten its PIN — 6982, and the
  // account password demanded anyway. Traced:
  //
  //   ins=87 p1=11 p2=9a sw=9000    the login signature
  //   ins=87 p1=11 p2=9d sw=6982    the keychain unlock, refused
  //
  // Nothing is given away by leaving it. Slot 9A still costs a button press for
  // every single use, which is the real gate; the PIN is theatre this applet
  // accepts from anyone. The window closes on its own.
  if (rc != 0) {
    LOG("sign failed: -0x%x", -rc);
    return append_sw(response, response_len, response_cap, 0x6f00);
  }

  if (!respond_dynamic_auth(sig, sig_len, response, response_len, response_cap)) {
    return false;
  }
  signature_until = now_ms() + SIGNATURE_ACK_MS;
  LOG("signed with slot %02x", apdu[3]);
  return true;
}

void piv_init(void) {
  // Derive a stable card GUID from the device MAC so macOS keeps recognising
  // the same card across reboots.
  pico_unique_board_id_t board_id;
  uint8_t device_hash[32];
  pico_get_unique_board_id(&board_id);
  mbedtls_sha256(board_id.id, sizeof(board_id.id), device_hash, 0);
  memcpy(CHUID_OBJECT + CHUID_GUID_OFFSET, device_hash, 16);
  CHUID_OBJECT[CHUID_GUID_OFFSET + 6] =
    (CHUID_OBJECT[CHUID_GUID_OFFSET + 6] & 0x0f) | 0x40;
  CHUID_OBJECT[CHUID_GUID_OFFSET + 8] =
    (CHUID_OBJECT[CHUID_GUID_OFFSET + 8] & 0x3f) | 0x80;

  const char *cert_9a_pem = NULL;
  const char *key_9a_pem = NULL;
  const char *cert_9d_pem = NULL;
  const char *key_9d_pem = NULL;
  key_source = "none";

  // The only source there is. Keys arrive one way — generated on this chip at
  // first boot — so there is nothing to prefer them over.
  if (storage_loaded()) {
    key_source = "flash";
    cert_9a_pem = storage_get(STORAGE_CERT_9A);
    key_9a_pem = storage_get(STORAGE_KEY_9A);
    cert_9d_pem = storage_get(STORAGE_CERT_9D);
    key_9d_pem = storage_get(STORAGE_KEY_9D);
  }

  if (piv_keys_initialized) {
    mbedtls_pk_free(&auth_key);
    mbedtls_pk_free(&key_mgmt_key);
  }
  mbedtls_pk_init(&auth_key);
  mbedtls_pk_init(&key_mgmt_key);
  piv_keys_initialized = true;

  cert_9a_der_len = 0;
  cert_9d_der_len = 0;

  if (!cert_9a_pem) {
    LOG("PIV identity is unconfigured; it is generated at first boot, so this "
        "means identity_generate() failed");
    return;
  }

  int rc = mbedtls_pk_parse_key(&auth_key, (const unsigned char *)key_9a_pem,
                                strlen(key_9a_pem) + 1, NULL, 0, NULL, NULL);
  key_parse_rc = rc;
  if (rc != 0) LOG("auth private key could not be loaded: -0x%x", -rc);

  rc = mbedtls_pk_parse_key(&key_mgmt_key, (const unsigned char *)key_9d_pem,
                            strlen(key_9d_pem) + 1, NULL, 0, NULL, NULL);
  if (rc != 0) LOG("key-management private key could not be loaded: -0x%x", -rc);

  decode_pem_cert(cert_9a_pem, cert_9a_der, sizeof(cert_9a_der), &cert_9a_der_len);
  decode_pem_cert(cert_9d_pem, cert_9d_der, sizeof(cert_9d_der), &cert_9d_der_len);
  LOG("PIV identity loaded from %s (9a cert %u bytes, 9d cert %u bytes)",
           key_source, (unsigned)cert_9a_der_len, (unsigned)cert_9d_der_len);
}

void piv_reload_keys(void) {
  cert_9a_der_len = 0;
  cert_9d_der_len = 0;
  pending_response_len = 0;
  pending_response_off = 0;
  piv_init();
}

bool piv_handle_apdu(const uint8_t *apdu, size_t apdu_len,
                     uint8_t *response, size_t *response_len,
                     size_t response_cap) {
  // Any APDU means a host is talking to the card, whoever it turns out to be.
  if (first_contact_ms == 0) first_contact_ms = now_ms();

  *response_len = 0;
  if (apdu_len < 4) {
    return append_sw(response, response_len, response_cap, 0x6700);
  }

  uint8_t ins = apdu[1];
  uint8_t cla = apdu[0];

  // Command chaining: macOS splits a GENERAL AUTHENTICATE that carries a
  // 2048-bit challenge across several APDUs.
  if ((cla & 0x10) && ins == 0x87) {
    const uint8_t *data = NULL;
    size_t data_len = 0;
    if (!read_lc_data(apdu, apdu_len, &data, &data_len) ||
        data_len > sizeof(chained_apdu_data)) {
      chained_apdu_data_len = 0;
      return append_sw(response, response_len, response_cap, 0x6700);
    }
    memcpy(chained_apdu_data, data, data_len);
    chained_apdu_data_len = data_len;
    chained_ins = ins;
    chained_p1 = apdu[2];
    chained_p2 = apdu[3];
    return append_sw(response, response_len, response_cap, 0x9000);
  }

  uint8_t chained_apdu[8 + sizeof(chained_apdu_data)];
  if (chained_apdu_data_len && ins == chained_ins && apdu[2] == chained_p1 && apdu[3] == chained_p2) {
    const uint8_t *data = NULL;
    size_t data_len = 0;
    if (!read_lc_data(apdu, apdu_len, &data, &data_len) ||
        chained_apdu_data_len + data_len > sizeof(chained_apdu_data)) {
      chained_apdu_data_len = 0;
      return append_sw(response, response_len, response_cap, 0x6700);
    }
    memcpy(chained_apdu_data + chained_apdu_data_len, data, data_len);
    chained_apdu_data_len += data_len;

    chained_apdu[0] = cla & (uint8_t)~0x10;
    chained_apdu[1] = ins;
    chained_apdu[2] = apdu[2];
    chained_apdu[3] = apdu[3];
    chained_apdu[4] = 0x00;
    chained_apdu[5] = (uint8_t)(chained_apdu_data_len >> 8);
    chained_apdu[6] = (uint8_t)chained_apdu_data_len;
    memcpy(chained_apdu + 7, chained_apdu_data, chained_apdu_data_len);
    apdu = chained_apdu;
    apdu_len = 7 + chained_apdu_data_len;
    chained_apdu_data_len = 0;
  } else if (chained_apdu_data_len) {
    chained_apdu_data_len = 0;
  }

  bool ok;
  switch (ins) {
    case 0xa4:
      ok = handle_select(apdu, apdu_len, response, response_len, response_cap);
      break;
    case 0xc0:
      ok = handle_get_response(apdu, apdu_len, response, response_len, response_cap);
      break;
    case 0xcb:
      ok = handle_get_data(apdu, apdu_len, response, response_len, response_cap);
      break;
    case 0x20:
      ok = handle_verify(apdu, apdu_len, response, response_len, response_cap);
      break;
    case 0x87:
      ok = handle_general_authenticate(apdu, apdu_len, response, response_len, response_cap);
      break;
    default:
      ok = append_sw(response, response_len, response_cap, 0x6d00);
      break;
  }

  uint16_t sw = 0;
  if (*response_len >= 2) {
    sw = (uint16_t)((response[*response_len - 2] << 8) | response[*response_len - 1]);
  }
  trace_apdu(cla, ins, apdu[2], apdu[3], sw);
  return ok;
}
