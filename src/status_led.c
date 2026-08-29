#include "status_led.h"

#include "board_config.h"

#if STATUS_LED_GPIO >= 0

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "ws2812.pio.h"

static PIO pio = pio0;
static uint state_machine = 0;

static void put_pixel(uint32_t red, uint32_t green, uint32_t blue) {
  // WS2812 wants GRB, most significant bit first, left-aligned in 32 bits.
  uint32_t grb = ((green & 0xff) << 16) | ((red & 0xff) << 8) | (blue & 0xff);
  pio_sm_put_blocking(pio, state_machine, grb << 8u);
}

static void show_level(uint32_t level) {
  put_pixel(STATUS_LED_COLOR_R * level, STATUS_LED_COLOR_G * level,
            STATUS_LED_COLOR_B * level);
}

void status_led_init(void) {
  uint offset = pio_add_program(pio, &ws2812_program);

  pio_gpio_init(pio, STATUS_LED_GPIO);
  pio_sm_set_consecutive_pindirs(pio, state_machine, STATUS_LED_GPIO, 1, true);

  pio_sm_config config = ws2812_program_get_default_config(offset);
  sm_config_set_sideset_pins(&config, STATUS_LED_GPIO);
  // 24 bits per pixel, shifted out MSB first, autopull at 24.
  sm_config_set_out_shift(&config, false, true, 24);
  sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);

  int cycles_per_bit = ws2812_T1 + ws2812_T2 + ws2812_T3;
  float divider = (float)clock_get_hz(clk_sys) / (800000.0f * cycles_per_bit);
  sm_config_set_clkdiv(&config, divider);

  pio_sm_init(pio, state_machine, offset, &config);
  pio_sm_set_enabled(pio, state_machine, true);

  // Brief ramp at boot. The indicator is otherwise dark unless a pinpad request
  // is pending, so without this there is no way to tell a correctly wired LED
  // from a broken one. It also clears whatever colour the previous firmware
  // latched — a WS2812 holds its last value indefinitely.
  for (int level = 0; level <= STATUS_LED_BRIGHTNESS; level += 2) {
    show_level((uint32_t)level);
    sleep_ms(4);
  }
  for (int level = STATUS_LED_BRIGHTNESS; level >= 0; level -= 2) {
    show_level((uint32_t)level);
    sleep_ms(4);
  }
  put_pixel(0, 0, 0);
}

// Three flashes of 180 ms, lit for the first half of each.
#define CONFIRM_FLASHES 3
#define CONFIRM_CYCLE_MS 180

void status_led_update(status_led_mode_t mode) {
  static status_led_mode_t shown = STATUS_LED_OFF;
  static bool lit;
  static uint32_t last_refresh;
  static uint32_t confirm_started;

  if (mode == STATUS_LED_OFF) {
    if (lit) {
      put_pixel(0, 0, 0);
      lit = false;
    }
    shown = mode;
    return;
  }

  uint32_t now = to_ms_since_boot(get_absolute_time());

  if (mode == STATUS_LED_ARMED) {
    // Steady, and written once rather than every pass: the WS2812 holds its
    // value, so repainting it only costs PIO traffic.
    if (shown != STATUS_LED_ARMED || !lit) {
      show_level(STATUS_LED_BRIGHTNESS);
      lit = true;
      shown = mode;
    }
    return;
  }

  if (mode == STATUS_LED_CONFIRM) {
    // Three flashes, timed from the moment the mode was entered. The pattern
    // runs once and then stays dark even if the caller keeps asking, so a long
    // acknowledgement window does not turn into a blinking light.
    if (shown != STATUS_LED_CONFIRM) {
      confirm_started = now;
      shown = mode;
    }
    uint32_t elapsed = now - confirm_started;
    bool want = elapsed < (CONFIRM_FLASHES * CONFIRM_CYCLE_MS) &&
                (elapsed % CONFIRM_CYCLE_MS) < (CONFIRM_CYCLE_MS / 2);
    if (want != lit) {
      show_level(want ? STATUS_LED_BRIGHTNESS : 0);
      lit = want;
    }
    return;
  }

  // ~50 Hz is plenty for a breath and keeps the PIO FIFO out of the main loop's
  // way the rest of the time.
  if (lit && shown == STATUS_LED_WAITING && (now - last_refresh) < 20) return;
  last_refresh = now;
  lit = true;
  shown = mode;

  // Triangle wave, squared for a roughly perceptual ramp. No floating point and
  // no trigonometry: at this size the difference from a sine is not visible.
  uint32_t phase = now % STATUS_LED_BREATHE_MS;
  uint32_t half = STATUS_LED_BREATHE_MS / 2;
  uint32_t rising = phase < half ? phase : (STATUS_LED_BREATHE_MS - phase);
  uint32_t level = (rising * 255) / half;
  level = (level * level) / 255;
  show_level((level * STATUS_LED_BRIGHTNESS) / 255);
}

#else

void status_led_init(void) {}
void status_led_update(status_led_mode_t mode) { (void)mode; }

#endif
