#include "config_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "mbedtls/base64.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "fingerprint.h"
#include "identity.h"
#include "piv.h"
#include "settings.h"
#include "storage.h"
#include "trace.h"
#include "tusb.h"

// One PROVISION_CHUNK line carries at most 480 base64 characters, so the
// command buffer only has to be a little larger than that.
#define COMMAND_CAP 640
#define CONFIG_WINDOW_MS (120u * 1000u)
#define PRESS_WAIT_MS 15000u

static char command[COMMAND_CAP];
static size_t command_len;
static uint32_t config_authorized_until;
static bool awaiting_press;

static uint32_t now_ms(void) {
  return to_ms_since_boot(get_absolute_time());
}

static bool window_open(uint32_t deadline, uint32_t window) {
  return deadline != 0 && (uint32_t)(deadline - now_ms()) <= window;
}

void config_console_send_line(const char *line) {
  const char *parts[] = {line, "\r\n"};
  for (size_t part = 0; part < 2; part++) {
    size_t length = strlen(parts[part]);
    size_t offset = 0;
    uint32_t started = now_ms();
    while (offset < length && (now_ms() - started) < 2000) {
      offset += tud_cdc_write(parts[part] + offset, (uint32_t)(length - offset));
      tud_cdc_write_flush();
      if (offset < length) {
        // Single cooperative loop: without pumping, the host never drains the
        // endpoint and this would spin until the deadline.
        tud_task();
        sleep_ms(1);
      }
    }
  }
  tud_cdc_write_flush();
}

static void send_line(const char *line) {
  config_console_send_line(line);
}

// Asks the operator to prove physical possession of the device.
//
// Blocks the main loop for up to PRESS_WAIT_MS, so it pumps USB itself. That is
// safe because the console is only ever reached from the main loop, never from
// inside a TinyUSB callback.
static bool demand_button_press(void) {
  send_line("PROMPT PRESS");
  awaiting_press = true;
  uint32_t deadline = now_ms() + PRESS_WAIT_MS;
  bool pressed = false;

  while ((int32_t)(now_ms() - deadline) < 0) {
    tud_task();
    if (button_pressed()) {
      pressed = true;
      break;
    }
    sleep_ms(2);
  }

  awaiting_press = false;
  if (!pressed) send_line("ERR NO_PRESS");
  return pressed;
}

bool config_console_awaiting_press(void) {
  return awaiting_press;
}

// Which silicon stepping this is, from the boot ROM version byte at 0x13.
//
// CHIP_ID.REVISION cannot answer this: A4 changed only the boot ROM, so it
// reports the same revision as A3. picotool reads this same byte for the same
// reason.
//
// It matters because three of the RP2350 Hacking Challenge findings are fixed in
// silicon and in no other way. On A2 a glitched chip can re-enable debug and read
// OTP (E16), boot unsigned code (E20), or have secure boot defeated with a laser
// (E24). No firmware closes any of them — so a unit's stepping is a security
// property, and this is how to read it off an assembled device.
static const char *chip_stepping(void) {
  uint8_t rom_version = *(const volatile uint8_t *)0x00000013;
  switch (rom_version) {
    case 2:  return "rp2350-a2";
    case 3:  return "rp2350-a3";
    case 4:  return "rp2350-a4";
    default: return "rp2350-unknown";
  }
}

static bool config_authorized(void) {
  return window_open(config_authorized_until, CONFIG_WINDOW_MS);
}

static bool require_config_authorization(void) {
  if (config_authorized()) return true;
  send_line("ERR CONFIG_LOCKED run=CONFIG_UNLOCK");
  return false;
}

static bool append_provision_chunk(char *arguments) {
  char *separator = strchr(arguments, ' ');
  if (!separator) return false;
  *separator = '\0';

  storage_slot_t slot = storage_slot_by_name(arguments);
  if (slot == STORAGE_SLOT_COUNT) return false;

  const unsigned char *encoded = (const unsigned char *)(separator + 1);
  size_t encoded_length = strlen(separator + 1);
  uint8_t decoded[480];
  size_t decoded_length = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_length,
                            encoded, encoded_length) != 0) {
    return false;
  }
  return storage_stage_append(slot, decoded, decoded_length);
}

static void handle_command(void) {
  // Sized for the STATUS line, which is the longest thing sent from here and
  // has grown twice. snprintf truncates in silence, so a field added without
  // room simply loses the tail of the line — which is where the device name
  // lives, and it looked like a quoting bug rather than a buffer one. The guard
  // below turns the next occurrence into an obvious error instead.
  char line[320];

  if (strcmp(command, "PING") == 0) {
    send_line("PONG");

  } else if (strcmp(command, "STATUS") == 0) {
    int status_len = snprintf(line, sizeof(line),
             "OK STATUS chip=%s presence=%s keys=%s source=%s alg=%s keyrc=-0x%04x pairing=%s config=%s aid=%s claimed=%s boothold=%s fp=%s touch=%s boot_rx=%u/%s lines=tx:%u/%u,rx:%u/%u,min=%uus name=\"%s\"",
             chip_stepping(),
             // What can authorise a signature. Without a sensor nothing can:
             // the button configures the device and never authenticates it.
             piv_module_mismatch() ? "blocked"
                                   : (fingerprint_present() ? "fingerprint" : "none"),
             piv_has_identity() ? "loaded" : "unconfigured",
             piv_key_source_name(), piv_algorithm_name(),
             (unsigned)(-piv_key_parse_error()),
             piv_pairing_mode_active() ? "on" : "off",
             config_authorized() ? "unlocked" : "locked",
             settings_aid_mode_name(settings_aid_mode()),
             piv_private_aid_selected() ? "yes" : "no",
             // Whether the button was down when the device booted. Reported so
             // the reset gesture can be verified without watching the LED.
             button_held_at_boot() ? "yes" : "no",
             fingerprint_status_text(),
             !fingerprint_touch_wired() ? "unwired"
                                        : (fingerprint_touch_asserted() ? "down" : "up"),
             (unsigned)fingerprint_boot_rx_bytes(),
             fingerprint_boot_saw_hello() ? "hello" : "nohello",
             (unsigned)fingerprint_line_high(false), (unsigned)fingerprint_line_edges(false),
             (unsigned)fingerprint_line_high(true), (unsigned)fingerprint_line_edges(true),
             (unsigned)fingerprint_line_min_pulse_us(),
             identity_common_name());
    if (status_len >= (int)sizeof(line)) {
      // Say so rather than sending a line that looks complete and is not.
      send_line("ERR STATUS truncated: grow line[]");
      return;
    }
    send_line(line);

  } else if (strcmp(command, "CONFIG_UNLOCK") == 0) {
    // A blank device has nothing to protect yet, so first-time setup does not
    // need a press. Once it holds an identity, reconfiguration costs physical
    // access.
    if (!piv_has_identity()) {
      config_authorized_until = now_ms() + CONFIG_WINDOW_MS;
      send_line("OK CONFIG_UNLOCK first_setup seconds=120");
    } else if (demand_button_press()) {
      config_authorized_until = now_ms() + CONFIG_WINDOW_MS;
      send_line("OK CONFIG_UNLOCK button seconds=120");
    }

  } else if (strcmp(command, "PROVISION_BEGIN") == 0) {
    if (!require_config_authorization()) return;
    storage_stage_reset();
    send_line("OK PROVISION_BEGIN");

  } else if (strncmp(command, "PROVISION_CHUNK ", 16) == 0) {
    if (!require_config_authorization()) return;
    send_line(append_provision_chunk(command + 16) ? "OK PROVISION_CHUNK"
                                                   : "ERR PROVISION_CHUNK");

  } else if (strcmp(command, "PROVISION_COMMIT") == 0) {
    if (!require_config_authorization()) return;
    if (storage_stage_commit()) {
      piv_reload_keys();
      send_line("OK PROVISION_COMMIT");
    } else {
      send_line("ERR PROVISION_COMMIT");
    }

    } else if (strcmp(command, "BENCH") == 0) {
    uint32_t ms = piv_benchmark_sign();
    if (ms) {
      snprintf(line, sizeof(line), "OK BENCH alg=%s sign_ms=%lu",
               piv_algorithm_name(), (unsigned long)ms);
    } else {
      snprintf(line, sizeof(line), "ERR BENCH no_key");
    }
    send_line(line);

  } else if (strcmp(command, "TRACE") == 0) {
    trace_dump(send_line);
    send_line("OK TRACE");

  } else if (strcmp(command, "TRACE_CLEAR") == 0) {
    trace_clear();
    send_line("OK TRACE_CLEAR");

  } else if (strcmp(command, "PAIRING_MODE") == 0) {
    if (demand_button_press()) {
      piv_set_pairing_mode(true);
      send_line("OK PAIRING_MODE seconds=120");
    } else {
      piv_set_pairing_mode(false);
    }

  } else if (strcmp(command, "PAIRING_MODE_OFF") == 0) {
    piv_set_pairing_mode(false);
    send_line("OK PAIRING_MODE_OFF");

  } else if (strncmp(command, "ENROLL", 6) == 0) {
    // Enrols a finger into a slot, defaulting to the next free one. Costs a
    // press first: adding a fingerprint is adding a way to use the key, so it
    // deserves the same physical proof as any other reconfiguration.
    if (!fingerprint_present()) {
      send_line("ERR ENROLL no_module");
      return;
    }
    unsigned slot = fingerprint_template_count();
    if (command[6] == ' ') slot = (unsigned)atoi(command + 7);
    if (!demand_button_press()) return;

    send_line("PROMPT FINGER place and lift, then place again");
    if (fingerprint_enroll((uint16_t)slot, 30000)) {
      snprintf(line, sizeof(line), "OK ENROLL slot=%u total=%u", slot,
               fingerprint_template_count());
      send_line(line);
    } else {
      send_line("ERR ENROLL");
    }

  } else if (strcmp(command, "FINGERPRINT_INFO") == 0) {
    // Read-only and reveals nothing secret, so no press gate.
    fp_info_t info;
    if (fingerprint_read_info(&info)) {
      snprintf(line, sizeof(line),
               "OK FINGERPRINT_INFO model=\"%s\" sw=\"%s\" mfr=\"%s\" sensor=\"%s\" "
               "addr=%08lx capacity=%u baud=%lu seclevel=%u pwd=%08lx table=%04x",
               info.product_model, info.sw_version, info.manufacturer, info.sensor_name,
               (unsigned long)info.device_address, info.capacity,
               (unsigned long)info.baud, info.security_level,
               (unsigned long)info.password, info.table_flag);
      send_line(line);
    } else {
      send_line("ERR FINGERPRINT_INFO");
    }

  } else if (strcmp(command, "FINGERPRINT_PROBE") == 0) {
    bool ok = fingerprint_probe();
    snprintf(line, sizeof(line),
             "%s FINGERPRINT_PROBE fp=%s baud=%u rx_bytes=%u hello=%s touch=%s",
             ok ? "OK" : "ERR", fingerprint_status_text(),
             (unsigned)fingerprint_probe_baud(),
             (unsigned)fingerprint_probe_rx_bytes(),
             fingerprint_probe_saw_hello() ? "yes" : "no",
             !fingerprint_touch_wired() ? "unwired"
                                        : (fingerprint_touch_asserted() ? "down" : "up"));
    send_line(line);

  } else if (strcmp(command, "FINGERPRINT_SN") == 0) {
    // The per-unit identity, and the one to bind against. Two modules from one
    // reel must differ here; if they do not, module binding is not possible at
    // all and the plan for it should be abandoned rather than half-built.
    uint8_t sn[FP_CHIP_SERIAL_LEN];
    if (!fingerprint_chip_serial(sn)) {
      send_line("ERR FINGERPRINT_SN");
    } else {
      int w = snprintf(line, sizeof(line), "OK FINGERPRINT_SN ");
      for (size_t i = 0; i < sizeof(sn); i++) {
        w += snprintf(line + w, sizeof(line) - (size_t)w, "%02x", sn[i]);
      }
      send_line(line);
    }

  } else if (strcmp(command, "FINGERPRINT_INFO_RAW") == 0) {
    // The field offsets in fingerprint_read_info() are guessed by scanning, not
    // documented. This prints the page so the guess can be checked against a
    // real module — which is the whole reason it exists.
    uint8_t page[512];
    uint16_t len = fingerprint_read_info_page(page, sizeof(page));
    if (len == 0) {
      send_line("ERR FINGERPRINT_INFO_RAW");
    } else {
      snprintf(line, sizeof(line), "OK FINGERPRINT_INFO_RAW bytes=%u", (unsigned)len);
      send_line(line);
      // 32 bytes a line keeps each one inside the console line buffer.
      for (uint16_t off = 0; off < len; off += 32) {
        uint16_t n = (uint16_t)(len - off);
        if (n > 32) n = 32;
        int w = snprintf(line, sizeof(line), "INFO %03u ", (unsigned)off);
        for (uint16_t i = 0; i < n; i++) {
          w += snprintf(line + w, sizeof(line) - (size_t)w, "%02x", page[off + i]);
        }
        send_line(line);
      }
    }

  } else if (strcmp(command, "FINGERPRINT_ERASE") == 0) {
    if (!demand_button_press()) return;
    send_line(fingerprint_erase_all() ? "OK FINGERPRINT_ERASE" : "ERR FINGERPRINT_ERASE");

  } else if (strcmp(command, "GENERATE_IDENTITY") == 0) {
    // Replaces the identity with a freshly generated one whose private key has
    // never left this chip. Destroys the old one, so anything paired to it
    // stops working until re-paired — hence the button press.
    if (!require_config_authorization()) return;
    if (!demand_button_press()) return;
    if (identity_generate()) {
      piv_reload_keys();
      snprintf(line, sizeof(line), "OK GENERATE_IDENTITY cn=\"%s\"",
               identity_common_name());
      send_line(line);
    } else {
      send_line("ERR GENERATE_IDENTITY");
    }

  // There is deliberately no FACTORY_RESET here. Erasing a user's credentials
  // is the one operation a host should not be able to start at all, so the only
  // path is the button held through power-up (see factory_reset_gesture() in
  // main.c). A press gate would not be enough: it makes the wipe *cost* a press,
  // but the host still chooses the moment, and a press the user believes is
  // authorising something else would serve.

  } else if (strcmp(command, "USB_RECONNECT") == 0) {
    send_line("OK USB_RECONNECT");
    sleep_ms(100);
    tud_disconnect();
    sleep_ms(500);
    tud_connect();

  } else if (strncmp(command, "AID_MODE ", 9) == 0) {
    // Switches which application the card answers, and therefore which driver
    // macOS binds. Exactly one driver owns a card and macOS chooses it at
    // insertion from the AID alone, so this is the only way to move between the
    // driverless and pinpad workflows.
    //
    // The reboot is not optional: the AID is answered during enumeration, so
    // the host has to be made to enumerate again.
    //
    // It costs a button press like any other reconfiguration. Otherwise host
    // software alone could move the device into a mode where a different driver
    // handles its authentication, with nothing physical to notice it.
    aid_mode_t wanted;
    if (strcmp(command + 9, "standard") == 0) {
      wanted = AID_MODE_STANDARD;
    } else if (strcmp(command + 9, "pinpad") == 0) {
      wanted = AID_MODE_PINPAD;
    } else {
      send_line("ERR AID_MODE usage=standard|pinpad");
      return;
    }

    if (wanted == settings_aid_mode()) {
      snprintf(line, sizeof(line), "OK AID_MODE %s unchanged",
               settings_aid_mode_name(wanted));
      send_line(line);
      return;
    }
    if (!demand_button_press()) return;
    if (!settings_set_aid_mode(wanted)) {
      send_line("ERR AID_MODE write_failed");
      return;
    }
    snprintf(line, sizeof(line), "OK AID_MODE %s rebooting",
             settings_aid_mode_name(wanted));
    send_line(line);
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);

  } else if (strcmp(command, "REBOOT") == 0) {
    send_line("OK REBOOT");
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);

  } else if (strcmp(command, "BOOTLOADER") == 0) {
    if (!require_config_authorization()) return;
    send_line("OK BOOTLOADER");
    sleep_ms(100);
    // The boot ROM handles this in hardware, so it lands in the bootloader
    // every time rather than depending on timing.
    reset_usb_boot(0, 0);

  } else {
    send_line("ERR UNKNOWN_COMMAND");
  }
}

void config_console_poll(void) {
  while (tud_cdc_available()) {
    char c;
    if (tud_cdc_read(&c, 1) != 1) break;
    if (c == '\r') continue;
    if (c == '\n') {
      command[command_len] = '\0';
      if (command_len) handle_command();
      command_len = 0;
    } else if (command_len + 1 < sizeof(command)) {
      command[command_len++] = c;
    }
  }
}

void config_console_init(void) {
  command_len = 0;
  config_authorized_until = 0;
}
