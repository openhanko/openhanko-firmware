#include "config_console.h"

#include <stdio.h>
#include <string.h>

#include "button.h"
#include "mbedtls/base64.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "piv.h"
#include "settings.h"
#include "storage.h"
#include "trace.h"
#include "tusb.h"

// One PROVISION_CHUNK line carries at most 480 base64 characters, so the
// command buffer only has to be a little larger than that.
#define COMMAND_CAP 640
#define CONFIG_WINDOW_MS (120u * 1000u)
#define ATTENTION_WINDOW_MS (30u * 1000u)
#define PRESS_WAIT_MS 15000u

static char command[COMMAND_CAP];
static size_t command_len;
static uint32_t config_authorized_until;
static uint32_t attention_until;
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

bool config_console_attention_active(void) {
  return window_open(attention_until, ATTENTION_WINDOW_MS);
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
  char line[192];

  if (strcmp(command, "PING") == 0) {
    send_line("PONG");

  } else if (strcmp(command, "STATUS") == 0) {
    snprintf(line, sizeof(line),
             "OK STATUS firmware=rp2040 presence=button keys=%s source=%s alg=%s keyrc=-0x%04x pairing=%s config=%s aid=%s claimed=%s",
             piv_has_identity() ? "loaded" : "unconfigured",
             piv_key_source_name(), piv_algorithm_name(),
             (unsigned)(-piv_key_parse_error()),
             piv_pairing_mode_active() ? "on" : "off",
             config_authorized() ? "unlocked" : "locked",
             settings_aid_mode_name(settings_aid_mode()),
             piv_private_aid_selected() ? "yes" : "no");
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

  } else if (strncmp(command, "ATTENTION ", 10) == 0) {
    // Driven by a macOS helper watching for the PIN prompt. The card itself
    // never hears from macOS until a PIN is submitted, so this is the only way
    // to signal before the user has already acted.
    if (strcmp(command + 10, "ON") == 0) {
      attention_until = now_ms() + ATTENTION_WINDOW_MS;
      send_line("OK ATTENTION ON seconds=30");
    } else if (strcmp(command + 10, "OFF") == 0) {
      attention_until = 0;
      send_line("OK ATTENTION OFF");
    } else {
      send_line("ERR ATTENTION");
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

  } else if (strcmp(command, "FACTORY_RESET") == 0) {
    if (!demand_button_press()) return;
    bool ok = storage_erase();
    piv_set_pairing_mode(false);
    piv_reload_keys();
    config_authorized_until = 0;
    send_line(ok ? "OK FACTORY_RESET" : "ERR FACTORY_RESET");

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
    // The RP2040 boot ROM handles this in hardware, so unlike the ESP32's
    // force-download-boot register it lands in the bootloader every time.
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
  attention_until = 0;
}
