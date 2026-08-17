#include "config_console.h"

#include <stdio.h>
#include <string.h>

#include "button.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "piv.h"
#include "trace.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "tusb.h"

static const char *TAG = "console";

// One PROVISION_CHUNK line carries at most 480 base64 characters, so the
// command buffer only has to be a little larger than that.
#define COMMAND_CAP 640
#define PROVISION_CAP 2400
#define CONFIG_WINDOW_US (120LL * 1000000LL)
#define PRESS_WAIT_MS 15000

static char command[COMMAND_CAP];
static size_t command_len;
static SemaphoreHandle_t cdc_write_mutex;
static int64_t config_authorized_until;
static volatile bool awaiting_press;

typedef struct {
  uint8_t data[PROVISION_CAP];
  size_t length;
} provision_buffer_t;

static provision_buffer_t provision_cert9a;
static provision_buffer_t provision_key9a;
static provision_buffer_t provision_cert9d;
static provision_buffer_t provision_key9d;

void config_console_send_line(const char *line) {
  if (cdc_write_mutex) xSemaphoreTake(cdc_write_mutex, portMAX_DELAY);
  const char *parts[] = {line, "\r\n"};
  for (size_t part = 0; part < 2; part++) {
    size_t length = strlen(parts[part]);
    size_t offset = 0;
    TickType_t started = xTaskGetTickCount();
    while (offset < length &&
           (xTaskGetTickCount() - started) < pdMS_TO_TICKS(2000)) {
      offset += tud_cdc_write(parts[part] + offset, length - offset);
      tud_cdc_write_flush();
      if (offset < length) vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  tud_cdc_write_flush();
  if (cdc_write_mutex) xSemaphoreGive(cdc_write_mutex);
}

static void send_line(const char *line) {
  config_console_send_line(line);
}

// Asks the operator to prove physical possession of the device. This is the
// button equivalent of the fingerprint check upstream runs before it will
// accept configuration changes.
static bool demand_button_press(void) {
  if (!button_claim(2000)) {
    send_line("ERR BUTTON_BUSY");
    return false;
  }
  button_flush();
  send_line("PROMPT PRESS");
  awaiting_press = true;
  bool pressed = button_wait_press(PRESS_WAIT_MS);
  awaiting_press = false;
  button_release();
  if (!pressed) send_line("ERR NO_PRESS");
  return pressed;
}

static bool config_authorized(void) {
  return esp_timer_get_time() < config_authorized_until;
}

static bool require_config_authorization(void) {
  if (config_authorized()) return true;
  send_line("ERR CONFIG_LOCKED run=CONFIG_UNLOCK");
  return false;
}

static void reset_provisioning(void) {
  provision_cert9a.length = 0;
  provision_key9a.length = 0;
  provision_cert9d.length = 0;
  provision_key9d.length = 0;
}

static provision_buffer_t *provision_buffer(const char *name) {
  if (strcmp(name, "cert9a") == 0) return &provision_cert9a;
  if (strcmp(name, "key9a") == 0) return &provision_key9a;
  if (strcmp(name, "cert9d") == 0) return &provision_cert9d;
  if (strcmp(name, "key9d") == 0) return &provision_key9d;
  return NULL;
}

static bool append_provision_chunk(char *arguments) {
  char *separator = strchr(arguments, ' ');
  if (!separator) return false;
  *separator = '\0';
  provision_buffer_t *buffer = provision_buffer(arguments);
  if (!buffer) return false;

  const unsigned char *encoded = (const unsigned char *)(separator + 1);
  size_t encoded_length = strlen(separator + 1);
  uint8_t decoded[480];
  size_t decoded_length = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded), &decoded_length,
                            encoded, encoded_length) != 0 ||
      buffer->length + decoded_length + 1 > sizeof(buffer->data)) return false;

  memcpy(buffer->data + buffer->length, decoded, decoded_length);
  buffer->length += decoded_length;
  buffer->data[buffer->length] = '\0';
  return true;
}

static bool provision_buffers_valid(void) {
  return provision_cert9a.length && provision_key9a.length &&
         provision_cert9d.length && provision_key9d.length &&
         strstr((char *)provision_cert9a.data, "BEGIN CERTIFICATE") &&
         strstr((char *)provision_key9a.data, "BEGIN PRIVATE KEY") &&
         strstr((char *)provision_cert9d.data, "BEGIN CERTIFICATE") &&
         strstr((char *)provision_key9d.data, "BEGIN PRIVATE KEY");
}

static bool commit_provisioning(void) {
  if (!provision_buffers_valid()) return false;
  nvs_handle_t handle;
  if (nvs_open("piv_keys", NVS_READWRITE, &handle) != ESP_OK) return false;

  esp_err_t result = nvs_set_blob(handle, "cert9a", provision_cert9a.data,
                                  provision_cert9a.length + 1);
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "key9a", provision_key9a.data, provision_key9a.length + 1);
  }
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "cert9d", provision_cert9d.data, provision_cert9d.length + 1);
  }
  if (result == ESP_OK) {
    result = nvs_set_blob(handle, "key9d", provision_key9d.data, provision_key9d.length + 1);
  }
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);

  if (result == ESP_OK) piv_reload_keys();
  return result == ESP_OK;
}

static bool factory_reset(void) {
  if (!demand_button_press()) return false;
  if (nvs_flash_erase() != ESP_OK || nvs_flash_init() != ESP_OK) return false;
  piv_set_pairing_mode(false);
  piv_reload_keys();
  config_authorized_until = 0;
  reset_provisioning();
  return true;
}

// A panic wipes the trace ring, so the reason the chip came back is the only
// evidence left that it went away at all. Without this, a firmware crash and a
// dropped BLE link are indistinguishable from the host.
static const char *reset_reason_name(void) {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB:      return "usb";
    case ESP_RST_EXT:      return "external";
    default:               return "other";
  }
}

static void handle_command(void) {
  char line[192];

  if (strcmp(command, "PING") == 0) {
    send_line("PONG");

  } else if (strcmp(command, "STATUS") == 0) {
    snprintf(line, sizeof(line),
             "OK STATUS firmware=simple presence=button keys=%s source=%s alg=%s keyrc=-0x%04x pairing=%s config=%s reset=%s uptime=%llus",
             piv_has_identity() ? "loaded" : "unconfigured",
             piv_key_source_name(), piv_algorithm_name(),
             (unsigned)(-piv_key_parse_error()),
             piv_pairing_mode_active() ? "on" : "off",
             config_authorized() ? "unlocked" : "locked",
             reset_reason_name(),
             (unsigned long long)(esp_timer_get_time() / 1000000));
    send_line(line);

  } else if (strcmp(command, "CONFIG_UNLOCK") == 0) {
    // A blank device has nothing to protect yet, so first-time setup does not
    // need a press. Once it holds an identity — provisioned or compiled in —
    // reconfiguration costs physical access.
    if (!piv_has_identity()) {
      config_authorized_until = esp_timer_get_time() + CONFIG_WINDOW_US;
      send_line("OK CONFIG_UNLOCK first_setup seconds=120");
    } else if (demand_button_press()) {
      config_authorized_until = esp_timer_get_time() + CONFIG_WINDOW_US;
      send_line("OK CONFIG_UNLOCK button seconds=120");
    }

  } else if (strcmp(command, "PROVISION_BEGIN") == 0) {
    if (!require_config_authorization()) return;
    reset_provisioning();
    send_line("OK PROVISION_BEGIN");

  } else if (strncmp(command, "PROVISION_CHUNK ", 16) == 0) {
    if (!require_config_authorization()) return;
    send_line(append_provision_chunk(command + 16) ? "OK PROVISION_CHUNK"
                                                   : "ERR PROVISION_CHUNK");

  } else if (strcmp(command, "PROVISION_COMMIT") == 0) {
    if (!require_config_authorization()) return;
    send_line(commit_provisioning() ? "OK PROVISION_COMMIT" : "ERR PROVISION_COMMIT");

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
    send_line(factory_reset() ? "OK FACTORY_RESET" : "ERR FACTORY_RESET");

  } else if (strcmp(command, "USB_RECONNECT") == 0) {
    send_line("OK USB_RECONNECT");
    vTaskDelay(pdMS_TO_TICKS(100));
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    tud_connect();

  } else if (strcmp(command, "REBOOT") == 0) {
    send_line("OK REBOOT");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();

  } else if (strcmp(command, "BOOTLOADER") == 0) {
    if (!require_config_authorization()) return;
    send_line("OK BOOTLOADER");
    vTaskDelay(pdMS_TO_TICKS(100));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();

  } else {
    send_line("ERR UNKNOWN_COMMAND");
  }
}

static void console_task(void *arg) {
  (void)arg;
  while (true) {
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
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool config_console_awaiting_press(void) {
  return awaiting_press;
}

void config_console_start(void) {
  command_len = 0;
  config_authorized_until = 0;
  cdc_write_mutex = xSemaphoreCreateMutex();
  xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
  ESP_LOGI(TAG, "provisioning console ready on the CDC interface");
}
