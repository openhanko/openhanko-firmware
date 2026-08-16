#pragma once

#include <stdint.h>

extern uint8_t const smart_card_hid_report_descriptor[];

void smart_card_init_serial(void);
