// Shared domain types.
#ifndef GENDA_COMMON_H
#define GENDA_COMMON_H

#include "core.h"

// raw_inputs.id. v == 0 means "no row": sqlite rowids are always > 0.
typedef struct {
  i64 v;
} RawId;

// Highest IMAP UID already ingested (meta.imap_last_uid).
typedef struct {
  i64 v;
} ImapUid;

// Closed set; the wire/SQL text lives in db.c.
typedef enum { KIND_NONE, KIND_APPOINTMENT, KIND_OBLIGATION } EventKind;

// Normalized input for both Android notifications and email. Filled only by
// a boundary parser (dbParseInput, imapParseInput): ext_id and time are
// always set.
typedef struct {
  char source[32], app[128], title[512], text[4096], from[256], time[64], ext_id[128];
} Input;

// Classification result. {0} (ok == 0) = not classified: Jev failed, retry
// the input later.
typedef struct {
  char title[512], starts_at[64], deadline[64], location[256];
  EventKind kind;
  f64 confidence;
  int is_event; // confident enough to become an events row
  int ok;       // 1 = Jev answered and every field parsed
} Classified;

#endif
