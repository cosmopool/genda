// genda - personal agenda server: ingest notifications+email, classify into
// events, serve msgpack feed. POSIX sockets, sqlite, libcurl.
#include "common.h"
#include "db.h"
#include "imap.h"
#include "server.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_port = 8080;
char g_db_path[1024] = "./genda.db";
char g_token[256] = "";
sqlite3 *g_db = NULL;

void mainLog(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[genda] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

const char *mainEnv(const char *k, const char *dflt) {
  const char *v = getenv(k);
  return (v && *v) ? v : dflt;
}

int main(void) {
  g_port = atoi(mainEnv("GENDA_PORT", "8080"));
  snprintf(g_db_path, sizeof g_db_path, "%s", mainEnv("GENDA_DB", "./genda.db"));
  snprintf(g_token, sizeof g_token, "%s", getenv("GENDA_TOKEN") ? getenv("GENDA_TOKEN") : "");
  curl_global_init(CURL_GLOBAL_ALL);
  if (dbOpen()) return 1;
  pthread_t imap;
  pthread_create(&imap, NULL, imapThread, NULL);
  pthread_detach(imap);
  return serverRun();
}
