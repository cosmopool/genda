// Shared types and process-wide state (defined in main.c).
#ifndef GENDA_COMMON_H
#define GENDA_COMMON_H

#include <sqlite3.h>

// Normalized input for both Android notifications and email.
typedef struct {
  char source[32], app[128], title[512], text[4096], from[256], time[64], ext_id[128];
} Input;

// Classification result. Mock = keyword heuristic (see classify.c TODO).
typedef struct {
  char title[512], starts_at[64], deadline[64], location[256], kind[16];
  double confidence;
} Classified;

// Process config + db handle.
extern int g_port;
extern char g_db_path[1024];
extern char g_token[256];
extern sqlite3 *g_db;

void mainLog(const char *fmt, ...);
const char *mainEnv(const char *k, const char *dflt);

#endif
