#pragma once

#include <stdint.h>

#include "tusb.h"

extern tusb_desc_device_t const smart_card_device_descriptor;
extern uint8_t const smart_card_fs_configuration_descriptor[];
extern uint8_t const smart_card_hid_report_descriptor[];
extern char const *smart_card_string_descriptors[];
extern int const smart_card_string_descriptor_count;

void smart_card_init_serial(void);
