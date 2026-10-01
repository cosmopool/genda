// SQLite persistence: raw inputs, classified events, IMAP progress.
#ifndef GENDA_DB_H
#define GENDA_DB_H

#include "common.h"
#include "msgpack.h"

int dbOpen(void);
void dbUtcNow(char *out, size_t n);
// Parse msgpack map body into Input. Unknown keys skipped. Missing ext_id is
// derived by hash of source/title/text, missing time is now. Returns 0 ok.
int dbParseInput(const unsigned char *body, long len, Input *in);
// Store raw input, dedupe by ext_id. Returns raw_id, {0} on error.
RawId dbStoreRaw(const Input *in);
// Idempotent: a raw_id that already has an event is left alone. Returns 0 ok.
int dbStoreEvent(RawId raw_id, const Classified *c);
// Pack all events in range as msgpack array. since/until "" = unbounded.
int dbPackEvents(const char *since, const char *until, MpWriter *w);
ImapUid dbMetaUid(void);
void dbMetaUidSet(ImapUid uid);

#endif
