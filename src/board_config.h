#pragma once

// Presence button.
//
// Set BUTTON_USE_BOOTSEL to 1 to use the BOOTSEL button instead of a wired one,
// which makes the board work with no extra hardware.
//
// It is not free: reading BOOTSEL means overriding the flash chip-select and
// disabling interrupts for tens of microseconds, because the pin is on the QSPI
// bus rather than the GPIO bank. Sampling is rate-limited to
// BUTTON_SAMPLE_MS to keep that off USB's back. Good for bench work; wire a
// real button for anything you intend to keep.
#define BUTTON_USE_BOOTSEL 0

//   momentary switch to GND   -> ACTIVE_LEVEL 0, PULL_UP 1
//   TTP223-style touch module -> ACTIVE_LEVEL 1, PULL_UP 0
//
// GP12 with the internal pull-up: wire the switch between GP12 and any GND pin.
// No external resistor needed.
//
// The button configures the device and never authenticates it. Its two jobs are
// the factory reset gesture and opening fingerprint enrolment, both of which are
// gated on something other than the click itself.
#ifndef BUTTON_GPIO
#define BUTTON_GPIO 12
#endif
#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_PULL_UP 1

// How often the pin is actually sampled. Matters mostly for BOOTSEL, where each
// sample costs an interrupt blackout.
#define BUTTON_SAMPLE_MS 20

// Time-based debounce: the main loop runs at whatever rate USB work allows, so
// counting samples would drift with load.
#define BUTTON_DEBOUNCE_MS 40
#define BUTTON_MIN_INTERVAL_MS 400

// Which AID a factory-fresh device answers. Runtime state after that — see
// settings.h — so one firmware image serves both workflows with no reflash.
//
// Standard is the default deliberately: an unopened box then works on any Mac
// with nothing installed. Pinpad is an opt-in upgrade the driver's installer
// switches the device into, and which the device abandons on its own if
// nothing claims it.
// Shown by macOS in its PIN prompt, as "Certificate For PIV Authentication
// (<this> <serial> authentication)". Keep it short: the wrapper text is
// Apple's and cannot be changed, so everything inside the parentheses is the
// only part that is ours.
#define DEVICE_NAME "OpenHanko"

#define PIV_DEFAULT_AID_MODE 0  /* AID_MODE_STANDARD */

// How long after the host first talks to the card to wait for our driver to
// claim it before giving up and reverting to the standard AID.
//
// macOS probes AIDs within about two seconds of enumeration, so this is
// generous. It only ever elapses on a Mac where the driver is absent, and the
// user sees the device work a moment after plugging in rather than not at all.
#define AID_REVERT_TIMEOUT_MS 10000

// HLK-ZW111 fingerprint module on UART1.
//
// GP4/GP5 are UART1's default pins and clear of the button on GP12. Set
// FINGERPRINT_UART_TX to -1 to build without sensor support at all — which
// yields a device that cannot authenticate anything, since a match is the only
// thing that authorises a signature. The module is detected at startup, so a
// configured pin with nothing attached is not an error, just an inert card.
// These name the MCU's own pins, not the module's, and the two cross over.
// GP4 is UART1 TX in silicon — an output — so it drives the module's RX input;
// GP5 is UART1 RX and listens to the module's TX output. Wiring TX to TX gives
// two outputs fighting each other and a module that never answers, which is
// indistinguishable from a dead module.
//
//     MCU GP4  (UART1 TX, output) ---> ZW111 pin 5  RX
//     MCU GP5  (UART1 RX, input)  <--- ZW111 pin 4  TX
//
#define FINGERPRINT_UART_INSTANCE uart1
#define FINGERPRINT_UART_TX 4   // MCU transmits here; goes to the module's RX
#define FINGERPRINT_UART_RX 5   // MCU receives here; comes from the module's TX
#define FINGERPRINT_BAUD 57600

// The module's TouchOut line (ZW111 pin 2), which asserts while a finger is on
// the sensor. -1 if it is not wired.
//
// Two things come of having it. Cheap: finger detection becomes a GPIO read
// instead of a PS_GetImage round trip over UART, so the idle poll stops holding
// a conversation with the module several times a second. And a real, if modest,
// security gain: a match arriving on TX while this line says nothing is
// touching the sensor did not come from a finger, so forging one means driving
// two lines in a plausible time relationship rather than replaying bytes on
// one. That is cost, not authentication — see THREAT-MODEL.md.
#define FINGERPRINT_TOUCH_GPIO 6

// Which level means "a finger is on the sensor". Active-high on the ZW111.
//
// The datasheet does not say so — it names the pin, calls it a wake IRQ, and
// leaves the level to Hi-Link's unpublished protocol note. Established by
// reading STATUS with and without a finger on the sensor, and confirmed by every
// authentication since: FINGERPRINT_REQUIRE_TOUCH discards a match unless this
// line agrees at both ends of the capture, so the wrong polarity here would mean
// nothing ever authenticates.
//
// That is also how a broken wire presents — closed, not open. The pin is pulled
// to the inactive level, so an unwired or disconnected TouchOut reads as "no
// finger" instead of floating, and STATUS reports the line as touch=.
#define FINGERPRINT_TOUCH_ACTIVE_LEVEL 1

// Whether a match is refused when the touch line disagrees.
//
// Kept separate from the pin definition so the correlation can be turned off
// without unwiring anything, which is what to do first if a board stops
// authenticating with the sensor otherwise responding.
#define FINGERPRINT_REQUIRE_TOUCH 1

// Optional discrete indicator, and -1 on the production board.
//
// A sealed case has nowhere to put one where anybody could see it, so the
// fingerprint module's own ring is the entire indicator: status_led.c compiles
// to no-ops and every indication goes through mirror_light() to the ring. That
// is the better arrangement anyway — the light the user reacts to sits on the
// surface they touch.
//
// Set to a GPIO to drive an addressable RGB LED there instead, as the
// development boards did. It is a WS2812, driven by PIO because the protocol
// needs sub-microsecond pulse widths the CPU cannot hit reliably alongside USB,
// and it latches its last value, so status_led_init() clears it at boot rather
// than inheriting whatever earlier firmware left lit.
#ifndef STATUS_LED_GPIO
#define STATUS_LED_GPIO -1
#endif
#define STATUS_LED_BRIGHTNESS 72

// What the ring shows at idle on a device that has never been told otherwise.
//
// A sealed unit on a dark desk is invisible unlit: the sensor is a 19 mm circle
// of the same plastic as everything around it, and being findable is the first
// thing the device has to do. Bright enough to irritate is the opposite failure,
// and there is no brightness control to split the difference with — PS_ControlBLN
// carries a function code, two colours and a cycle count, and the colour is a
// three-bit mask. So the only dimmer is how many of the three dies are lit, and
// one is the quietest thing that is still visible.
//
// The value is that mask: bit 0 blue, bit 1 green, bit 2 red, 0 for off. This is
// only the factory default; IDLE_LIGHT on the console overrides it and the
// choice persists. A factory reset brings it back to this.
#define FINGERPRINT_IDLE_DEFAULT_COLOR 1  /* blue */
// Colour weights, multiplied by the brightness ramp.
#define STATUS_LED_COLOR_R 0
#define STATUS_LED_COLOR_G 1
#define STATUS_LED_COLOR_B 0
#define STATUS_LED_BREATHE_MS 1600

// The PIV PIN macOS insists on collecting. Not a secret, and not your account
// password: the button press is the real gate.
// How many digits the device types when a host asks for a PIN.
//
// The digits themselves are random and generated fresh for every prompt, so
// there is no value to define here. The card accepts any PIN — handle_verify()
// discards the bytes — so a fixed one would carry no more meaning than a random
// one, and would teach the user a number that looks like it means something.
//
// Six is the PIV minimum and what hosts expect.
#define PIV_DUMMY_PIN_DIGITS 6
