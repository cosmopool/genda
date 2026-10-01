// SQLite persistence: raw inputs, classified events, IMAP progress.
#ifndef GENDA_DB_H
#define GENDA_DB_H

#include "common.h"
#include "msgpack.h"

int dbOpen(void);
void utcNow(char *out, size_t n);
// Parse msgpack map body into Input. Unknown keys skipped. Returns 0 ok.
int parseInput(const unsigned char *body, long len, Input *in);
// Store raw input, dedupe by ext_id. Returns raw_id (>0) or -1 on error.
long long storeRaw(const Input *in);
int storeEvent(long long raw_id, const Classified *c);
// Pack all events in range as msgpack array. since/until "" = unbounded.
int packEvents(const char *since, const char *until, MpWriter *w);
long metaUid(void);
void metaUidSet(long uid);

#endif
