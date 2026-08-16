#include "status_led.h"

#include <math.h>

#include "board_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if STATUS_LED_GPIO >= 0

#include "led_strip.h"

static const char *TAG = "status_led";
static const int UPDATE_MS = 20;

static led_strip_handle_t strip;
static status_led_mode_t (*current_mode)(void);

static void show(uint8_t level) {
  if (!strip) return;
  led_strip_set_pixel(strip, 0,
                      (uint32_t)STATUS_LED_COLOR_R * level,
                      (uint32_t)STATUS_LED_COLOR_G * level,
                      (uint32_t)STATUS_LED_COLOR_B * level);
  led_strip_refresh(strip);
}

static void status_led_task(void *arg) {
  (void)arg;
  float phase = 0.0f;
  uint8_t shown = 0;

  while (true) {
    status_led_mode_t mode = current_mode ? current_mode() : STATUS_LED_OFF;
    uint8_t level = 0;

    if (mode == STATUS_LED_SOLID) {
      level = STATUS_LED_BRIGHTNESS;
      phase = 0.0f;
    } else if (mode == STATUS_LED_BREATHE) {
      phase += 2.0f * (float)M_PI * UPDATE_MS / STATUS_LED_BREATHE_MS;
      if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      // Raised cosine: starts and ends at zero, so the breath has no visible
      // step when it begins or ends.
      level = (uint8_t)((1.0f - cosf(phase)) * 0.5f * STATUS_LED_BRIGHTNESS);
    } else {
      phase = 0.0f;
    }

    if (level != shown) {
      show(level);
      shown = level;
    }
    vTaskDelay(pdMS_TO_TICKS(UPDATE_MS));
  }
}

void status_led_init(status_led_mode_t (*mode)(void)) {
  current_mode = mode;

  led_strip_config_t strip_config = {
    .strip_gpio_num = STATUS_LED_GPIO,
    .max_leds = 1,
    .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    .led_model = LED_MODEL_WS2812,
    .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 10 * 1000 * 1000,
    .flags.with_dma = false,
  };

  esp_err_t result = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "no WS2812 on GPIO%d: %s", STATUS_LED_GPIO, esp_err_to_name(result));
    strip = NULL;
    return;
  }

  // Clear whatever the previous firmware latched into the LED.
  led_strip_clear(strip);
  xTaskCreate(status_led_task, "status_led", 2560, NULL, 2, NULL);
  ESP_LOGI(TAG, "status LED on GPIO%d", STATUS_LED_GPIO);
}

#else  // STATUS_LED_GPIO < 0

void status_led_init(status_led_mode_t (*mode)(void)) {
  (void)mode;
}

#endif
