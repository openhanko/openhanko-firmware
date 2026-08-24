#include "button.h"

#include "board_config.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

#if BUTTON_USE_BOOTSEL
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#endif

static bool stable_down;
static bool candidate_down;
static uint32_t candidate_since;
static uint32_t last_press;
static uint32_t last_sample;
static bool last_level;
static bool held_at_boot;

static uint32_t now_ms(void) {
  return to_ms_since_boot(get_absolute_time());
}

#if BUTTON_USE_BOOTSEL

// BOOTSEL sits on the QSPI bus, not the GPIO bank, so reading it means briefly
// driving the flash chip-select low and watching the pin. Flash is unusable
// while that happens, hence the RAM residency and the interrupt lockout.
static bool __no_inline_not_in_flash_func(read_bootsel)(void) {
  const uint cs_pin = 1;
  uint32_t flags = save_and_disable_interrupts();

  hw_write_masked(&ioqspi_hw->io[cs_pin].ctrl,
                  GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

  // The pin needs a moment to settle once the pull is removed.
  for (volatile int i = 0; i < 1000; i++) {}

  bool pressed = !(sio_hw->gpio_hi_in & (1u << cs_pin));

  hw_write_masked(&ioqspi_hw->io[cs_pin].ctrl,
                  GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

  restore_interrupts(flags);
  return pressed;
}

#endif

// Sampled at most every BUTTON_SAMPLE_MS. The main loop calls button_pressed()
// thousands of times a second, and on the BOOTSEL path each real sample costs
// an interrupt blackout, so unthrottled polling would visibly disturb USB.
static bool raw_down(void) {
  uint32_t now = now_ms();
  if (last_sample != 0 && (now - last_sample) < BUTTON_SAMPLE_MS) {
    return last_level;
  }
  last_sample = now;
#if BUTTON_USE_BOOTSEL
  last_level = read_bootsel();
#else
  last_level = gpio_get(BUTTON_GPIO) == BUTTON_ACTIVE_LEVEL;
#endif
  return last_level;
}

void button_init(void) {
#if !BUTTON_USE_BOOTSEL
  gpio_init(BUTTON_GPIO);
  gpio_set_dir(BUTTON_GPIO, GPIO_IN);
#if BUTTON_PULL_UP
  gpio_pull_up(BUTTON_GPIO);
#else
  gpio_pull_down(BUTTON_GPIO);
#endif
  // Settle the pull before sampling, so a floating pin does not read as a press
  // during the first few milliseconds.
  sleep_ms(5);
#endif

  last_sample = 0;
  stable_down = raw_down();
  held_at_boot = stable_down;
  candidate_down = stable_down;
  candidate_since = now_ms();
}

bool button_held_at_boot(void) {
  return held_at_boot;
}

bool button_is_down(void) {
  return raw_down();
}

bool button_pressed(void) {
  uint32_t now = now_ms();
  bool level = raw_down();

  if (level != candidate_down) {
    candidate_down = level;
    candidate_since = now;
    return false;
  }

  if (level == stable_down || (now - candidate_since) < BUTTON_DEBOUNCE_MS) {
    return false;
  }

  stable_down = level;
  if (!level) return false;

  // Ignore a second press inside the rate limit, which is usually contact
  // bounce that outlasted the debounce window rather than a real tap.
  if (last_press != 0 && (now - last_press) < BUTTON_MIN_INTERVAL_MS) return false;
  last_press = now;
  return true;
}
