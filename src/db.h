// SQLite persistence: raw inputs, classified events, IMAP progress.
#ifndef GENDA_DB_H
#define GENDA_DB_H

#include "common.h"
#include "msgpack.h"

int dbOpen(void);
void dbUtcNow(char *out, size_t n);
// Parse msgpack map body into Input. Unknown keys skipped. Returns 0 ok.
int dbParseInput(const unsigned char *body, long len, Input *in);
// Store raw input, dedupe by ext_id (derived by hash when empty).
// Returns raw_id (>0) or -1 on error.
long long dbStoreRaw(const Input *in);
// Idempotent: a raw_id that already has an event is left alone. Returns 0 ok.
int dbStoreEvent(long long raw_id, const Classified *c);
// Pack all events in range as msgpack array. since/until "" = unbounded.
int dbPackEvents(const char *since, const char *until, MpWriter *w);
long dbMetaUid(void);
void dbMetaUidSet(long uid);

#endif
