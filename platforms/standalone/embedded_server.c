/***********************************************************************
 Freeciv - Copyright (C) 2026 - The Freeciv Project
 SPDX-License-Identifier: GPL-2.0-or-later
***********************************************************************/
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "embedded_server.h"

static pthread_t server_thread;
static int thread_created;
static atomic_int running;
static atomic_int stop_requested;
static atomic_int exit_status;
static struct fc_embedded_config server_config;
static unsigned char *initial_data;

struct engine_exit_handler {
  void (*callback)(void);
  struct engine_exit_handler *next;
};

static struct engine_exit_handler *exit_handlers;
static pthread_mutex_t exit_handlers_mutex = PTHREAD_MUTEX_INITIALIZER;

extern unsigned char fc_embedded_data_start[], fc_embedded_data_end[];
extern unsigned char fc_embedded_bss_start[], fc_embedded_bss_end[];

#if defined(__SANITIZE_ADDRESS__)
/* This operation copies an ELF section, including padding and ASan's global
 * redzones. Keep the shadow memory intact and leave normal game accesses
 * instrumented. Volatile byte accesses avoid libc's memcpy interception. */
__attribute__((no_sanitize_address, no_sanitize_undefined))
static void engine_image_copy(void *destination, const void *source, size_t size)
{
  volatile unsigned char *out = destination;
  const volatile unsigned char *in = source;

  while (size-- > 0) {
    *out++ = in != NULL ? *in++ : 0;
  }
}
#else
static void engine_image_copy(void *destination, const void *source, size_t size)
{
  if (source != NULL) {
    memcpy(destination, source, size);
  } else {
    memset(destination, 0, size);
  }
}
#endif

static int reset_engine(void)
{
  size_t data_size = (uintptr_t) fc_embedded_data_end
                    - (uintptr_t) fc_embedded_data_start;
  size_t bss_size = (uintptr_t) fc_embedded_bss_end
                   - (uintptr_t) fc_embedded_bss_start;

  if (initial_data == NULL) {
    initial_data = malloc(data_size ? data_size : 1);
    if (initial_data == NULL) {
      return -1;
    }
    engine_image_copy(initial_data, fc_embedded_data_start, data_size);
  }
  engine_image_copy(fc_embedded_data_start, initial_data, data_size);
  engine_image_copy(fc_embedded_bss_start, NULL, bss_size);
  return 0;
}

static char *copy_string(const char *text)
{
  char *copy;

  if (text == NULL) {
    return NULL;
  }
  copy = malloc(strlen(text) + 1);
  if (copy != NULL) {
    strcpy(copy, text);
  }
  return copy;
}

static void free_config(void)
{
  free((void *) server_config.ruleset);
  free((void *) server_config.script);
  free((void *) server_config.savefile);
  free((void *) server_config.saves);
  free((void *) server_config.scenarios);
  free((void *) server_config.logfile);
  memset(&server_config, 0, sizeof(server_config));
}

static void server_finished(void *unused)
{
  struct engine_exit_handler *handler;

  (void) unused;
  for (;;) {
    pthread_mutex_lock(&exit_handlers_mutex);
    handler = exit_handlers;
    if (handler != NULL) {
      exit_handlers = handler->next;
    }
    pthread_mutex_unlock(&exit_handlers_mutex);
    if (handler == NULL) {
      break;
    }
    handler->callback();
    free(handler);
  }
  atomic_store(&running, 0);
}

static void *run_server(void *unused)
{
  (void) unused;
  pthread_cleanup_push(server_finished, NULL);
  fc_embedded_run(&server_config);
  pthread_cleanup_pop(1);
  return NULL;
}

int fc_embedded_start(const struct fc_embedded_config *config)
{
  pthread_attr_t attributes;
  int result;

  if (config == NULL || config->port < 1 || config->port > 65535
      || fc_embedded_running()) {
    return -1;
  }
  fc_embedded_stop();
  if (reset_engine() != 0) {
    return -1;
  }
  server_config.port = config->port;

#define COPY_FIELD(field)                                                \
  server_config.field = copy_string(config->field);                       \
  if (config->field != NULL && server_config.field == NULL) {             \
    free_config();                                                       \
    return -1;                                                           \
  }
  COPY_FIELD(ruleset);
  COPY_FIELD(script);
  COPY_FIELD(savefile);
  COPY_FIELD(saves);
  COPY_FIELD(scenarios);
  COPY_FIELD(logfile);
#undef COPY_FIELD

  atomic_store(&stop_requested, 0);
  atomic_store(&exit_status, 0);
  atomic_store(&running, 1);
  result = pthread_attr_init(&attributes);
  if (result == 0) {
    result = pthread_attr_setstacksize(&attributes, 4 * 1024 * 1024);
    if (result == 0) {
      result = pthread_create(&server_thread, &attributes, run_server, NULL);
    }
    pthread_attr_destroy(&attributes);
  }
  if (result != 0) {
    atomic_store(&running, 0);
    free_config();
    return -1;
  }
  thread_created = 1;
  return 0;
}

int fc_embedded_running(void)
{
  return atomic_load(&running);
}

int fc_embedded_should_stop(void)
{
  return atomic_load(&stop_requested);
}

int fc_embedded_stop(void)
{
  if (thread_created) {
    atomic_store(&stop_requested, 1);
    /* The server checks this flag in its input loop. Joining allows an
     * in-progress AI phase and asynchronous save to finish safely. */
    pthread_join(server_thread, NULL);
    thread_created = 0;
    free_config();
  }
  return fc_embedded_status();
}

int fc_embedded_status(void)
{
  return atomic_load(&exit_status);
}

void fc_embedded_exit(int status)
{
  atomic_store(&exit_status, status);
  pthread_exit((void *) (intptr_t) status);
}

int fc_embedded_atexit(void (*callback)(void))
{
  struct engine_exit_handler *handler = malloc(sizeof(*handler));

  if (handler == NULL) {
    return -1;
  }
  /* Run the engine's exit handlers in reverse registration order when its
   * thread finishes, while its private data is still alive. */
  handler->callback = callback;
  pthread_mutex_lock(&exit_handlers_mutex);
  handler->next = exit_handlers;
  exit_handlers = handler;
  pthread_mutex_unlock(&exit_handlers_mutex);
  return 0;
}

void fc_embedded_emergency_exit(int status)
{
  fc_embedded_exit(status);
}

char *fc_embedded_strtok(char *text, const char *delimiters)
{
  static _Thread_local char *position;

  /* libc strtok() can otherwise share its cursor between client and engine. */
  return strtok_r(text, delimiters, &position);
}
