// Process-wide state, logging, and env config (defined in config.c).
#ifndef GENDA_CONFIG_H
#define GENDA_CONFIG_H

#include <sqlite3.h>

extern int g_port;
extern char g_db_path[1024];
extern char g_token[256];
extern sqlite3 *g_db;

void configLog(const char *fmt, ...);
const char *configEnv(const char *k, const char *dflt);

#endif
