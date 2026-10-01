// SQLite persistence: raw inputs, classified events, IMAP progress.
#ifndef GENDA_DB_H
#define GENDA_DB_H

#include "common.h"
#include "msgpack.h"

int db_open(void);
void utc_now(char *out, size_t n);
// Parse msgpack map body into Input. Unknown keys skipped. Returns 0 ok.
int parse_input(const unsigned char *body, long len, Input *in);
// Store raw input, dedupe by ext_id. Returns raw_id (>0) or -1 on error.
long long store_raw(const Input *in);
int store_event(long long raw_id, const Classified *c);
// Pack all events in range as msgpack array. since/until "" = unbounded.
int pack_events(const char *since, const char *until, MPW *w);
long meta_uid(void);
void meta_uid_set(long uid);

#endif
