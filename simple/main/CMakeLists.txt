# piv.c includes secrets.h unconditionally, so that the compiler records it as a
# real dependency and editing it always forces a rebuild. Conditional inclusion
# via __has_include does not: creating secrets.h after a build that lacked it
# leaves a stale object file and silently produces a firmware with no keys.
#
# Seed it from the example on first configure. The firmware detects placeholder
# content and ignores it, so a seeded file behaves exactly like no file at all.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
  configure_file("${CMAKE_CURRENT_LIST_DIR}/secrets.example.h"
                 "${CMAKE_CURRENT_LIST_DIR}/secrets.h" COPYONLY)
endif()

idf_component_register(
  SRCS
    "main.c"
    "button.c"
    "config_console.c"
    "piv.c"
    "status_led.c"
    "trace.c"
    "usb_ccid.c"
    "usb_descriptors.c"
    "usb_hid.c"
  INCLUDE_DIRS "."
  REQUIRES bootloader_support driver esp_timer esp_tinyusb mbedtls nvs_flash
)
