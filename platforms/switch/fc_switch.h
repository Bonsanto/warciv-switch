/***********************************************************************
 Freeciv - Copyright (C) 2026 - The Freeciv Project
 SPDX-License-Identifier: GPL-2.0-or-later
***********************************************************************/
#ifndef FC__SWITCH_H
#define FC__SWITCH_H

#include <stddef.h>

struct SDL_Window;

int fc_switch_main(int argc, char **argv);
void fc_switch_poll(struct SDL_Window *window);
unsigned int fc_switch_mouse_buttons(void);
int fc_switch_keyboard(const char *initial, char *text, size_t size, int password);

#endif /* FC__SWITCH_H */
