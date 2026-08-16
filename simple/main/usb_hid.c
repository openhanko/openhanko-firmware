#include "usb_hid.h"

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_descriptors.h"

static const char *TAG = "usb_hid";
static const uint8_t ascii_to_keycode[128][2] = {HID_ASCII_TO_KEYCODE};

// Upstream spins here forever. A host that never polls the HID endpoint would
// wedge the presence task, so bound the wait instead.
static bool wait_hid_ready(uint32_t timeout_ms) {
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (!tud_hid_ready()) {
    if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

static bool send_key(uint8_t modifier, uint8_t keycode) {
  uint8_t report[6] = {keycode, 0, 0, 0, 0, 0};
  if (!wait_hid_ready(500)) return false;
  tud_hid_keyboard_report(0, modifier, report);
  vTaskDelay(pdMS_TO_TICKS(7));
  if (!wait_hid_ready(500)) return false;
  tud_hid_keyboard_report(0, 0, NULL);
  vTaskDelay(pdMS_TO_TICKS(7));
  return true;
}

bool usb_hid_type_line(const char *text) {
  for (const char *cursor = text; *cursor; cursor++) {
    uint8_t character = (uint8_t)*cursor;
    if (character >= 128 || ascii_to_keycode[character][1] == 0) {
      ESP_LOGW(TAG, "character 0x%02x is not typeable", character);
      return false;
    }
    uint8_t modifier = ascii_to_keycode[character][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
    if (!send_key(modifier, ascii_to_keycode[character][1])) return false;
  }
  return send_key(0, HID_KEY_ENTER);
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return smart_card_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;
}
