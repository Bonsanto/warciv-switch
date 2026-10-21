/***********************************************************************
 Freeciv - Copyright (C) 2026 - The Freeciv Project
 SPDX-License-Identifier: GPL-2.0-or-later
***********************************************************************/
#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include "fc_prehdrs.h"

#include <stdio.h>
#include <time.h>

#include "embedded_server.h"
#include "game.h"

/* Runs the actual largepox engine twice while the client-side libfreeciv
 * contains sentinel state. A shared game, RNG, callback table or ruleset
 * cache would either overwrite this state or fail on the second run. */
int main(int argc, char **argv)
{
  struct fc_embedded_config config = {0};
  const struct timespec delay = {0, 10000000};
  int iteration;

  if (argc != 3) {
    fprintf(stderr, "Usage: %s AUTOGAME_SCRIPT SAVE_DIRECTORY\n", argv[0]);
    return 2;
  }
  config.port = 15556;
  config.ruleset = "classic-warclient";
  config.script = argv[1];
  config.saves = argv[2];
  config.scenarios = argv[2];

  game.info.turn = 424242;
  i_am_client();
  for (iteration = 0; iteration < 2; iteration++) {
    time_t deadline = time(NULL) + 60;

    if (fc_embedded_start(&config) != 0) {
      fprintf(stderr, "Could not start embedded engine\n");
      return 1;
    }
    while (fc_embedded_running() && time(NULL) < deadline) {
      nanosleep(&delay, NULL);
    }
    if (fc_embedded_running()) {
      fprintf(stderr, "Embedded autogame timed out\n");
      fc_embedded_stop();
      return 1;
    }
    if (fc_embedded_stop() != 0 || game.info.turn != 424242 || is_server()) {
      fprintf(stderr, "Embedded engine failed or modified client state\n");
      return 1;
    }
  }
  puts("Embedded engine: two independent games completed; client state intact.");
  return 0;
}
