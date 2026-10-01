// SQLite persistence: raw inputs, classified events, IMAP progress.
#ifndef GENDA_DB_H
#define GENDA_DB_H

#include "common.h"
#include "msgpack.h"

int dbOpen(void);
void dbUtcNow(char *out, usize n);
// Parse msgpack map body into Input. Unknown keys skipped. Missing ext_id is
// derived by hash of source/title/text, missing time is now. Returns 0 ok.
int dbParseInput(const u8 *body, i64 len, Input *in);

typedef struct {
  RawId id; // {0} on db error (logged)
  int is_event;
} Ingest;

// The one ingest path (/ingest and IMAP): store raw input (dedupe by ext_id),
// classify, store its event once per raw_id.
Ingest dbIngest(const Input *in);
// Pack all events in range as msgpack array. since/until "" = unbounded.
int dbPackEvents(const char *since, const char *until, MpWriter *w);
ImapUid dbMetaUid(void);
void dbMetaUidSet(ImapUid uid);

#endif
