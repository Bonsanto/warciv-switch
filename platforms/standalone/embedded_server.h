/***********************************************************************
 Freeciv - Copyright (C) 2026 - The Freeciv Project
 SPDX-License-Identifier: GPL-2.0-or-later
***********************************************************************/
#ifndef FC__EMBEDDED_SERVER_H
#define FC__EMBEDDED_SERVER_H

/* This boundary deliberately contains no Freeciv game types. The embedded
 * server owns a separate copy of libfreeciv, including all global state. */
struct fc_embedded_config {
  int port;
  const char *ruleset;
  const char *script;
  const char *savefile;
  const char *saves;
  const char *scenarios;
  const char *logfile;
};

int fc_embedded_start(const struct fc_embedded_config *config);
int fc_embedded_running(void);
int fc_embedded_stop(void);
int fc_embedded_status(void);

/* Private entry points shared with the isolated server object. */
void fc_embedded_run(const struct fc_embedded_config *config);
int fc_embedded_should_stop(void);
void fc_embedded_exit(int status) __attribute__((noreturn));
void fc_embedded_emergency_exit(int status) __attribute__((noreturn));
int fc_embedded_atexit(void (*callback)(void));
char *fc_embedded_strtok(char *text, const char *delimiters);

#endif /* FC__EMBEDDED_SERVER_H */
