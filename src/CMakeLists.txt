cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico CACHE STRING "Board type")

# Must be set before pico_sdk_init(), which is when the SDK's pico_mbedtls
# CMakeLists reads it and stamps MBEDTLS_CONFIG_FILE onto pico_mbedtls_headers.
set(PICO_MBEDTLS_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls_config.h")

include(pico_sdk_import.cmake)
project(openhanko C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Bench-only module register writes, off in anything anyone flashes.
#
# FINGERPRINT_REG can set the module's encryption level, which the manual says
# cannot be changed once set. That is the only irreversible thing a host can
# reach in this firmware, so it is kept out of the firmware a host normally
# talks to rather than gated inside it.
option(FINGERPRINT_LAB_TOOLS "Expose FINGERPRINT_REG for module bring-up" OFF)

add_executable(openhanko
  otp.c
  main.c
  button.c
  config_console.c
  fingerprint.c
  identity.c
  piv.c
  status_led.c
  settings.c
  storage.c
  trace.c
  usb_ccid.c
  usb_descriptors.c
  usb_hid.c
)

target_include_directories(openhanko PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# Assembles ws2812.pio into ws2812.pio.h.
target_compile_definitions(openhanko PRIVATE
  FINGERPRINT_LAB_TOOLS=$<BOOL:${FINGERPRINT_LAB_TOOLS}>)

if(FINGERPRINT_LAB_TOOLS)
  message(WARNING "FINGERPRINT_LAB_TOOLS is ON: this build can permanently set "
                  "the fingerprint module's encryption level. Do not ship it.")
endif()

# -Wundef, on our own translation units only.
#
# An undefined macro in #if evaluates to 0 silently, so renaming a config macro
# and missing one use site compiles cleanly and changes behaviour — which is
# exactly how the idle light came to be forced off at the one place meant to
# switch it on. The SDK uses that pattern deliberately and everywhere, so the
# warning is worth nothing applied globally and worth having applied here.
set_source_files_properties(
  otp.c main.c button.c config_console.c fingerprint.c identity.c piv.c
  status_led.c settings.c storage.c trace.c usb_ccid.c usb_descriptors.c
  usb_hid.c
  PROPERTIES COMPILE_OPTIONS "-Wundef")

pico_generate_pio_header(openhanko ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_link_libraries(openhanko
  pico_bootrom
  pico_stdlib
  pico_unique_id
  pico_rand
  hardware_uart
  hardware_flash
  hardware_watchdog
  hardware_pio
  hardware_sync
  tinyusb_device
  tinyusb_board
  pico_mbedtls_crypto
  # X.509 writing, for the self-signed certificate generated in identity.c.
  # The SDK splits mbedTLS into crypto/x509/tls targets and pico_mbedtls_crypto
  # alone has no certificate writer, which shows up only at link time.
  pico_mbedtls_x509
  pico_mbedtls_headers
)

# Double-tap RESET is the bootrom's, not the SDK's.
#
# pico_bootsel_via_double_reset used to provide it: the application stashed a
# magic in the watchdog scratch and a second reset caught it. Enabling secure
# boot stops that working, and the reason it stops is the reason it was never
# enough — it needs the firmware to run, which is not when a recovery path is
# wanted. It also cost 800 ms of every boot.
#
# BOOT_FLAGS1.DOUBLE_TAP does the same job in the bootrom, before any
# application, so it rescues a board whose firmware is broken, unsigned or
# absent. bootkeys.py burns it during stage 1; see THREAT-MODEL.md.
#
# The consequence for a board with nothing burned: no double-tap at all, so use
# the BOOTSEL button or short QSPI_SS.

pico_enable_stdio_uart(openhanko 1)
pico_enable_stdio_usb(openhanko 0)

pico_add_extra_outputs(openhanko)
