#pragma once

#include <stdbool.h>

// Types text followed by Enter on the HID keyboard interface. Used only to
// deliver the dummy PIV PIN into the macOS smart-card prompt.
bool usb_hid_type_line(const char *text);
