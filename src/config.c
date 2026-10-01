// Process-wide state, logging, and env config.
#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int g_port = 8080;
char g_db_path[1024] = "./genda.db";
char g_token[256] = "";
sqlite3 *g_db = NULL;

void configLog(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[genda] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

const char *configEnv(const char *k, const char *dflt) {
  const char *v = getenv(k);
  return (v && *v) ? v : dflt;
}
