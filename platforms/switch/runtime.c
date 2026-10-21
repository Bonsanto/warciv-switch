/***********************************************************************
 Freeciv - Copyright (C) 2026 - The Freeciv Project
 SPDX-License-Identifier: GPL-2.0-or-later
***********************************************************************/
#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include "fc_prehdrs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>
#include <SDL2/SDL.h>

#include "client_main.h"
#include "connectdlg_common.h"
#include "embedded_server.h"
#include "fc_switch.h"
#include "mapctrl_common.h"
#include "zoom.h"

static PadState controller;
static Uint32 last_poll;
static Uint32 last_pan;
static Uint32 last_direction;
static Uint32 mouse_buttons;
static float pointer_x = 640;
static float pointer_y = 360;

unsigned int fc_switch_mouse_buttons(void)
{
  return mouse_buttons;
}

static void key_event(SDL_Keycode key, int down, SDL_Keymod modifiers)
{
  SDL_Event event;

  SDL_zero(event);
  event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
  event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
  event.key.keysym.sym = key;
  event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
  event.key.keysym.mod = modifiers;
  SDL_PushEvent(&event);
}

static void mouse_event(Uint8 button, int down)
{
  SDL_Event event;

  SDL_zero(event);
  event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
  event.button.button = button;
  event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
  event.button.clicks = 1;
  event.button.x = (int) pointer_x;
  event.button.y = (int) pointer_y;
  if (down) {
    mouse_buttons |= SDL_BUTTON(button);
  } else {
    mouse_buttons &= ~SDL_BUTTON(button);
  }
  SDL_PushEvent(&event);
}

static void shutdown_switch(void)
{
  fc_embedded_stop();
  socketExit();
  romfsExit();
}

static int startup_error(const char *message, Result result)
{
  consoleInit(NULL);
  printf("Warciv\n\n%s: 0x%08x\n\nPulsa + para salir.\n", message, result);
  while (appletMainLoop()) {
    padUpdate(&controller);
    if (padGetButtonsDown(&controller) & HidNpadButton_Plus) {
      break;
    }
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  return EXIT_FAILURE;
}

int fc_switch_main(int argc, char **argv)
{
  Result result;
  char **arguments;
  int i;

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&controller);
  result = romfsInit();
  if (R_FAILED(result)) {
    return startup_error("No se pudieron cargar los datos del juego", result);
  }
  result = socketInitializeDefault();
  if (R_FAILED(result)) {
    romfsExit();
    return startup_error("No se pudo iniciar la comunicacion local", result);
  }
  atexit(shutdown_switch);

  mkdir("sdmc:/switch", 0777);
  mkdir("sdmc:/switch/warciv", 0777);
  mkdir("sdmc:/switch/warciv/saves", 0777);
  mkdir("sdmc:/switch/warciv/scenarios", 0777);
  setenv("FREECIV_DATA_PATH", "sdmc:/switch/warciv/data;romfs:/data", 1);
  setenv("FREECIV_LOCAL_ENCODING", "UTF-8", 1);

  arguments = calloc((size_t) argc + 5, sizeof(*arguments));
  if (arguments == NULL) {
    return EXIT_FAILURE;
  }
  arguments[0] = argv[0];
  arguments[1] = "--tiles";
  arguments[2] = "trident";
  arguments[3] = "--name";
  arguments[4] = "Jugador";
  for (i = 1; i < argc; i++) {
    arguments[i + 4] = argv[i];
  }
  return client_main(argc + 4, arguments, FALSE);
}

int fc_switch_keyboard(const char *initial, char *text, size_t size, int password)
{
  SwkbdConfig keyboard;
  Result result = swkbdCreate(&keyboard, 0);

  if (R_FAILED(result)) {
    return 0;
  }
  swkbdConfigMakePresetDefault(&keyboard);
  swkbdConfigSetInitialText(&keyboard, initial != NULL ? initial : "");
  swkbdConfigSetStringLenMax(&keyboard, (u32) ((size - 1) / 4));
  if (password) {
    swkbdConfigSetPasswordFlag(&keyboard, 1);
  }
  result = swkbdShow(&keyboard, text, size);
  swkbdClose(&keyboard);
  /* Applet input must not become a click in the game on return. */
  if (mouse_buttons & SDL_BUTTON_LMASK) {
    mouse_event(SDL_BUTTON_LEFT, 0);
  }
  if (mouse_buttons & SDL_BUTTON_RMASK) {
    mouse_event(SDL_BUTTON_RIGHT, 0);
  }
  padUpdate(&controller);
  last_poll = SDL_GetTicks();
  return R_SUCCEEDED(result);
}

void fc_switch_poll(SDL_Window *window)
{
  HidAnalogStickState stick;
  Uint32 now = SDL_GetTicks();
  float elapsed = last_poll ? (float) (now - last_poll) / 1000.0f : 0.0f;
  u64 down, up, held;
  const u64 direction_buttons = HidNpadButton_Left | HidNpadButton_Right
                                | HidNpadButton_Up | HidNpadButton_Down;
  int width, height;

  last_poll = now;
  if (!appletMainLoop()) {
    SDL_Event event;

    SDL_zero(event);
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
    return;
  }
  padUpdate(&controller);
  down = padGetButtonsDown(&controller);
  up = padGetButtonsUp(&controller);
  held = padGetButtons(&controller);
  SDL_GetWindowSize(window, &width, &height);
  stick = padGetStickPos(&controller, 0);
  if (abs(stick.x) > 5000 || abs(stick.y) > 5000) {
    float speed = held & HidNpadButton_ZL ? 180.0f : 650.0f;

    if (elapsed > 0.05f) {
      elapsed = 0.05f;
    }
    pointer_x += (float) stick.x / 32767.0f * speed * elapsed;
    pointer_y -= (float) stick.y / 32767.0f * speed * elapsed;
    pointer_x = SDL_clamp(pointer_x, 0, width - 1);
    pointer_y = SDL_clamp(pointer_y, 0, height - 1);
    SDL_WarpMouseInWindow(window, (int) pointer_x, (int) pointer_y);
  } else {
    int x, y;

    SDL_GetMouseState(&x, &y);
    pointer_x = (float) x;
    pointer_y = (float) y;
  }
  if (down & HidNpadButton_A) {
    mouse_event(SDL_BUTTON_LEFT, 1);
  }
  if (up & HidNpadButton_A) {
    mouse_event(SDL_BUTTON_LEFT, 0);
  }
  if (down & HidNpadButton_X) {
    mouse_event(SDL_BUTTON_RIGHT, 1);
  }
  if (up & HidNpadButton_X) {
    mouse_event(SDL_BUTTON_RIGHT, 0);
  }
  if (down & HidNpadButton_B) {
    key_event(SDLK_ESCAPE, 1, KMOD_NONE);
    key_event(SDLK_ESCAPE, 0, KMOD_NONE);
  }
  if (down & HidNpadButton_Y) {
    key_event(SDLK_w, 1, KMOD_NONE);
    key_event(SDLK_w, 0, KMOD_NONE);
  }
  if (down & HidNpadButton_Plus) {
    key_event(SDLK_LSHIFT, 1, KMOD_SHIFT);
    key_event(SDLK_RETURN, 1, KMOD_SHIFT);
    key_event(SDLK_RETURN, 0, KMOD_SHIFT);
    key_event(SDLK_LSHIFT, 0, KMOD_NONE);
  }
  if (client_state() == C_S_RUNNING) {
    if (down & HidNpadButton_Minus) {
      send_save_game("quicksave");
    }
    if (down & HidNpadButton_L) {
      zoom_step_down();
    }
    if (down & HidNpadButton_R) {
      zoom_step_up();
    }
    stick = padGetStickPos(&controller, 1);
    if (now - last_pan >= 120 && (abs(stick.x) > 10000 || abs(stick.y) > 10000)) {
      enum direction8 direction;

      if (abs(stick.x) > abs(stick.y)) {
        direction = stick.x > 0 ? DIR8_EAST : DIR8_WEST;
      } else {
        direction = stick.y > 0 ? DIR8_NORTH : DIR8_SOUTH;
      }
      scroll_mapview(direction);
      last_pan = now;
    }
  }
  if ((down & direction_buttons) ||
      ((held & direction_buttons) && now - last_direction >= 180)) {
    const SDL_Keycode directions[3][3] = {
      {SDLK_HOME, SDLK_UP, SDLK_PAGEUP},
      {SDLK_LEFT, SDLK_UNKNOWN, SDLK_RIGHT},
      {SDLK_END, SDLK_DOWN, SDLK_PAGEDOWN}
    };
    int horizontal = !!(held & HidNpadButton_Right) - !!(held & HidNpadButton_Left);
    int vertical = !!(held & HidNpadButton_Down) - !!(held & HidNpadButton_Up);
    SDL_Keycode direction = directions[vertical + 1][horizontal + 1];
    if (direction != SDLK_UNKNOWN) {
      key_event(direction, 1, KMOD_NONE);
      key_event(direction, 0, KMOD_NONE);
    }
    last_direction = now;
  }
  SDL_Delay(8);
}
