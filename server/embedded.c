/***********************************************************************
 Freeciv - Copyright (C) 2026 - The Freeciv Project
 SPDX-License-Identifier: GPL-2.0-or-later
***********************************************************************/
#ifdef HAVE_CONFIG_H
#include <fc_config.h>
#endif

#include "fc_prehdrs.h"

#include "capstr.h"
#include "embedded_server.h"
#include "sernet.h"
#include "srv_main.h"

void fc_embedded_run(const struct fc_embedded_config *config)
{
  srv_init();
  init_our_capability();

  srvarg.bind_addr = "127.0.0.1";
  srvarg.port = config->port;
  srvarg.announce = ANNOUNCE_NONE;
  srvarg.metaserver_no_send = TRUE;
  srvarg.auth_enabled = FALSE;
  srvarg.fcdb_enabled = FALSE;
  srvarg.quitidle = 0;
  srvarg.exit_on_end = TRUE;
  srvarg.ruleset = (char *) config->ruleset;
  srvarg.script_filename = (char *) config->script;
  srvarg.log_filename = (char *) config->logfile;
  srvarg.saves_pathname = (char *) config->saves;
  srvarg.scenarios_pathname = (char *) config->scenarios;
  if (config->savefile != NULL) {
    sz_strlcpy(srvarg.load_filename, config->savefile);
  }

  srv_main();
}
