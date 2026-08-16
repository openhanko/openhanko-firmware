#include "ble_transport.h"
#include "board_config.h"
#include "bootloader_random.h"
#include "button.h"
#include "config_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "piv.h"
#include "status_led.h"
#include "usb_ccid.h"
#include "usb_hid.h"

static const char *TAG = "main";

// Solid for a moment after a signature, breathing whenever the device is
// waiting for a press, dark otherwise.
static status_led_mode_t led_mode(void) {
  if (piv_recent_signature()) return STATUS_LED_SOLID;
  if (piv_challenge_active() || config_console_awaiting_press() ||
      config_console_attention_active()) {
    return STATUS_LED_BREATHE;
  }
  return STATUS_LED_OFF;
}

// The entire authorization story of this proof of concept: a debounced button
// press opens a one-shot signing window for the PIV authentication key, then
// types the dummy PIN so the macOS smart-card prompt gets out of the way.
static void presence_task(void *arg) {
  (void)arg;

  while (true) {
    if (!button_claim(1000)) continue;
    bool pressed = button_wait_press(200);
    button_release();

    if (!pressed) {
      // Yield so a config console command can take the button between polls.
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    ESP_LOGI(TAG, "button pressed: authorizing PIV and typing the dummy PIN");
    config_console_send_line("EVENT PRESS");
    piv_note_user_presence();
    if (!usb_hid_type_line(PIV_DUMMY_PIN)) {
      ESP_LOGW(TAG, "HID interface was not ready; PIN not typed");
      config_console_send_line("EVENT PIN_NOT_TYPED");
    }
  }
}

void app_main(void) {
  esp_err_t nvs_result = nvs_flash_init();
  if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_result);

  // esp_random() is only guaranteed to be a true random source while Wi-Fi or
  // Bluetooth is running, and this firmware runs neither. Without this call the
  // hardware RNG is seeded once at boot and thereafter is just a PRNG, which is
  // what mbedTLS gets handed for RSA blinding in piv_rng().
  //
  // Bluetooth gives the hardware RNG a genuine entropy source, so with the radio
  // enabled there is nothing to do. Without it, esp_random() is only seeded once
  // at boot, and the SAR ADC source has to be turned on by hand — but the two
  // contend for the same ADC, hence the either/or.
  //
  // It matters more than it looks. PKCS#1 v1.5 padding is deterministic, so weak
  // randomness mostly costs side-channel resistance there; with ECDSA a
  // predictable nonce leaks the private key outright. Signing is RFC 6979
  // deterministic for that reason, which takes the RNG out of the path either
  // way. See UPSTREAM-REVIEW.md finding 6.
#if CONFIG_BT_ENABLED
  ESP_LOGI(TAG, "Bluetooth is enabled; the radio feeds the hardware RNG");
#else
  bootloader_random_enable();
  ESP_LOGI(TAG, "hardware RNG entropy source enabled");
#endif

  button_init();
  status_led_init(led_mode);
  piv_init();
  usb_ccid_start(piv_handle_apdu);
  config_console_start();

#if CONFIG_BT_ENABLED
  // The same applet, reachable over the air. Wired and wireless share
  // piv_handle_apdu() so they cannot drift apart.
  ble_transport_start(piv_handle_apdu);
#endif

  xTaskCreate(presence_task, "presence", 4096, NULL, 4, NULL);

  // No tud_task() loop here: esp_tinyusb runs the TinyUSB stack on its own
  // task, and calling tud_task() from a second task races it.
}
