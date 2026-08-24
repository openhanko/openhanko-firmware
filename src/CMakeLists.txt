cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico CACHE STRING "Board type")

# Must be set before pico_sdk_init(), which is when the SDK's pico_mbedtls
# CMakeLists reads it and stamps MBEDTLS_CONFIG_FILE onto pico_mbedtls_headers.
set(PICO_MBEDTLS_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls_config.h")

include(pico_sdk_import.cmake)
project(openhanko C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# secrets.h is optional user content, but the include in piv.c is unconditional
# so the compiler records it as a dependency. Seed it from the example when
# missing; the firmware detects placeholder content and ignores it. __has_include
# is the wrong tool here: it records no dependency, so creating secrets.h after a
# build that lacked it leaves a stale object file and flashes a keyless firmware.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
  configure_file("${CMAKE_CURRENT_LIST_DIR}/secrets.example.h"
                 "${CMAKE_CURRENT_LIST_DIR}/secrets.h" COPYONLY)
endif()

add_executable(openhanko
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
pico_generate_pio_header(openhanko ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)

target_link_libraries(openhanko
  pico_stdlib
  pico_unique_id
  pico_rand
  hardware_uart
  hardware_flash
  hardware_watchdog
  hardware_pio
  # QMK-style magic boot: tap RESET twice to enter the bootloader, so
  # reflashing needs neither BOOTSEL nor an unlocked console.
  pico_bootsel_via_double_reset
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

# stdio goes to UART only. USB belongs to the smart card, and routing printf
# through it would fight TinyUSB for the same peripheral.
# Double-tap RESET within this window to land in the bootloader. The library
# No activity LED configured: that option drives a plain GPIO, which would
# corrupt the WS2812 data line rather than light it.
target_compile_definitions(openhanko PRIVATE
  PICO_BOOTSEL_VIA_DOUBLE_RESET_TIMEOUT_MS=800
)

pico_enable_stdio_uart(openhanko 1)
pico_enable_stdio_usb(openhanko 0)

pico_add_extra_outputs(openhanko)
