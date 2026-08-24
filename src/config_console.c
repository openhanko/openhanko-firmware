#include "config_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "fingerprint.h"
#include "otp.h"
#include "identity.h"
#include "piv.h"
#include "settings.h"
#include "trace.h"
#include "tusb.h"

// Longest thing anyone sends is `AID_MODE standard`. The buffer is far larger
// than that so a host pasting junk into the port is truncated rather than
// running off the end.
#define COMMAND_CAP 128
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
             "OK STATUS chip=%s presence=%s keys=%s source=%s alg=%s keyrc=-0x%04x config=%s aid=%s claimed=%s boothold=%s button=%s fp=%s touch=%s otp=%s boot_rx=%u/%s lines=tx:%u/%u,rx:%u/%u,min=%uus name=\"%s\"",
             chip_stepping(),
             // What can authorise a signature. Without a sensor nothing can:
             // the button configures the device and never authenticates it.
             piv_module_mismatch() ? "blocked"
                                   : (fingerprint_present() ? "fingerprint" : "none"),
             piv_has_identity() ? "loaded" : "unconfigured",
             piv_key_source_name(), piv_algorithm_name(),
             (unsigned)(-piv_key_parse_error()),
             config_authorized() ? "unlocked" : "locked",
             settings_aid_mode_name(settings_aid_mode()),
             piv_private_aid_selected() ? "yes" : "no",
             // Whether the button was down when the device booted. Reported so
             // the reset gesture can be verified without watching the LED.
             button_held_at_boot() ? "yes" : "no",
             // Live pin state. A gate that keeps reporting NO_PRESS is either a
             // person who did not press or a wire that came off, and those are
             // not distinguishable from the other end of a console.
             button_is_down() ? "down" : "up",
             fingerprint_status_text(),
             !fingerprint_touch_wired() ? "unwired"
                                        : (fingerprint_touch_asserted() ? "down" : "up"),
             otp_secret_present() ? "set" : "none",
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

  } else if (strcmp(command, "TRACE") == 0) {
    trace_dump(send_line);
    send_line("OK TRACE");

  } else if (strcmp(command, "TRACE_CLEAR") == 0) {
    trace_clear();
    send_line("OK TRACE_CLEAR");

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

  } else if (strcmp(command, "OTP_STATUS") == 0) {
    uint32_t chipid = 0;
    bool readable = otp_selftest(&chipid);
    char fp[16];
    if (otp_secret_fingerprint(fp, sizeof(fp))) {
      snprintf(line, sizeof(line), "OK OTP_STATUS readable=%s chipid=%08lx secret=set id=%s",
               readable ? "yes" : "no", (unsigned long)chipid, fp);
    } else {
      snprintf(line, sizeof(line), "OK OTP_STATUS readable=%s chipid=%08lx secret=none",
               readable ? "yes" : "no", (unsigned long)chipid);
    }
    send_line(line);

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

  } else if (strcmp(command, "FINGERPRINT_SECPROBE") == 0) {
    // Read-only, and the answer decides whether the sensor link can ever be
    // authenticated on this part. See fingerprint_security_probe().
    uint8_t data[64];
    uint16_t data_len = 0;
    uint8_t control = 0xff;
    uint8_t cc = fingerprint_security_probe(data, sizeof(data), &data_len, &control);
    // The control is the whole test. 0xE2 answering 0x00 means nothing unless an
    // opcode that does not exist answers differently.
    const char *reading =
        cc == 0xff       ? "no_module" :
        cc == control    ? "same_as_unknown_opcode" :
        cc == 0x00       ? "implemented" :
        cc == 0x31       ? "implemented_wrong_level" :
        cc == 0x2e       ? "implemented_no_key" :
        cc == 0x01       ? "not_implemented" : "unexpected";
    int w = snprintf(line, sizeof(line),
                     "OK FINGERPRINT_SECPROBE cc=%02x control=%02x %s bytes=%u ",
                     cc, control, reading, (unsigned)data_len);
    for (uint16_t i = 0; i < data_len && w < (int)sizeof(line) - 3; i++) {
      w += snprintf(line + w, sizeof(line) - (size_t)w, "%02x", data[i]);
    }
    send_line(line);

#if FINGERPRINT_LAB_TOOLS
  } else if (strncmp(command, "FINGERPRINT_REG ", 16) == 0) {
    // Writes one module register. Bench builds only: -DFINGERPRINT_LAB_TOOLS=ON.
    //
    // This is the one irreversible, host-reachable thing in the firmware, which
    // is why it is not in the firmware anyone ships. Register 7 is the
    // encryption level and the manual is explicit that "changes are not allowed
    // after setting" — a module set to a level whose protocol we cannot speak
    // has no way back and no way to verify a fingerprint, because levels 2 and
    // above also refuse PS_Search, PS_StoreChar and PS_AutoEnroll.
    //
    // The confirmation codes make a non-destructive ladder possible, and it is
    // worth climbing before setting anything for real:
    //
    //   FINGERPRINT_REG 200 0   expect 1a — proves the module validates the
    //                           register number rather than acking any write
    //   FINGERPRINT_REG 7 5     expect 1b — 5 is Reserved in the level table, so
    //                           a refusal proves register 7 exists *and* has a
    //                           value table, without changing it
    //   FINGERPRINT_REG 7 3     the real thing, AES-128, one way only
    //
    // If step one returns 00, this module acks writes it does not understand and
    // nothing below it means anything.
    char *sep = strchr(command + 16, ' ');
    if (!sep) {
      send_line("ERR FINGERPRINT_REG usage=<reg> <value>");
      return;
    }
    *sep = '\0';
    unsigned long reg = strtoul(command + 16, NULL, 0);
    unsigned long value = strtoul(sep + 1, NULL, 0);
    if (reg > 255 || value > 255) {
      send_line("ERR FINGERPRINT_REG range");
      return;
    }
    // Register 4 is the baud rate. Writing it would strand the module on a rate
    // this firmware does not open, which is a way to lose a module without
    // learning anything.
    if (reg == 4) {
      send_line("ERR FINGERPRINT_REG baud_rate_refused");
      return;
    }
    if (!require_config_authorization()) return;
    if (!demand_button_press()) return;
    uint8_t cc = fingerprint_write_register((uint8_t)reg, (uint8_t)value);
    const char *meaning =
        cc == 0x00 ? "written" :
        cc == 0x1a ? "no_such_register" :
        cc == 0x1b ? "value_not_allowed" :
        cc == 0x18 ? "module_flash_error" :
        cc == 0x01 ? "packet_error" :
        cc == 0xff ? "no_module" : "unexpected";
    snprintf(line, sizeof(line), "OK FINGERPRINT_REG reg=%lu value=%lu cc=%02x %s",
             reg, value, cc, meaning);
    send_line(line);
#endif

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
    // The manual documents the info page's fields but not their offsets, so the
    // ones in fingerprint_read_info() were recovered from a real module. This
    // prints the page so they can be re-checked against another one — which is
    // the whole reason it exists, and how the original four-byte error was
    // found.
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
