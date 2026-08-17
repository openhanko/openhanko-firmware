#pragma once

#include <stdbool.h>

void config_console_start(void);

// True while a console command is asking the operator to press the button.
bool config_console_awaiting_press(void);

// Writes one line to the CDC console. Command replies start with OK/ERR;
// anything the device volunteers on its own starts with EVENT or PROMPT.
void config_console_send_line(const char *line);
