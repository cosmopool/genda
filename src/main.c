// genda - personal agenda server: ingest notifications+email, classify into
// events, serve msgpack feed. POSIX sockets, sqlite, libcurl.
#include "common.h"
#include "config.h"
#include "db.h"
#include "imap.h"
#include "server.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  g_port = atoi(configEnv("GENDA_PORT", "8080"));
  snprintf(g_db_path, sizeof g_db_path, "%s", configEnv("GENDA_DB", "./genda.db"));
  snprintf(g_token, sizeof g_token, "%s", configEnv("GENDA_TOKEN", ""));
  curl_global_init(CURL_GLOBAL_ALL);
  if (dbOpen()) return 1;
  pthread_t imap;
  pthread_create(&imap, NULL, imapThread, NULL);
  pthread_detach(imap);
  return serverRun();
}
