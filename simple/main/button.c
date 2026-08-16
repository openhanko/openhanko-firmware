#include "button.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "button";

static QueueHandle_t press_queue;
static SemaphoreHandle_t claim_mutex;

static bool button_is_down(void) {
  return gpio_get_level(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL;
}

static void button_task(void *arg) {
  (void)arg;
  int stable_samples = 0;
  bool press_reported = false;
  TickType_t last_press = 0;

  while (true) {
    if (button_is_down()) {
      if (stable_samples < BUTTON_DEBOUNCE_SAMPLES) stable_samples++;
    } else {
      stable_samples = 0;
      press_reported = false;
    }

    if (stable_samples >= BUTTON_DEBOUNCE_SAMPLES && !press_reported) {
      press_reported = true;
      TickType_t now = xTaskGetTickCount();
      if ((TickType_t)(now - last_press) >= pdMS_TO_TICKS(BUTTON_MIN_INTERVAL_MS)) {
        last_press = now;
        uint32_t event = (uint32_t)now;
        xQueueOverwrite(press_queue, &event);
        ESP_LOGI(TAG, "press");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

void button_init(void) {
  gpio_config_t io = {
    .pin_bit_mask = 1ULL << BUTTON_GPIO,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = BUTTON_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    .pull_down_en = BUTTON_PULL_UP ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&io));

  press_queue = xQueueCreate(1, sizeof(uint32_t));
  claim_mutex = xSemaphoreCreateMutex();
  xTaskCreate(button_task, "button", 2560, NULL, 5, NULL);
  ESP_LOGI(TAG, "presence button on GPIO%d, active %s", BUTTON_GPIO,
           BUTTON_ACTIVE_LEVEL ? "high" : "low");
}

bool button_claim(uint32_t timeout_ms) {
  return claim_mutex && xSemaphoreTake(claim_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void button_release(void) {
  if (claim_mutex) xSemaphoreGive(claim_mutex);
}

void button_flush(void) {
  uint32_t event;
  while (press_queue && xQueueReceive(press_queue, &event, 0) == pdTRUE) {}
}

bool button_wait_press(uint32_t timeout_ms) {
  uint32_t event;
  return press_queue &&
         xQueueReceive(press_queue, &event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
