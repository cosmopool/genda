// SQLite persistence: raw inputs, classified events, IMAP progress.
#include "db.h"

#include "classify.h"
#include "common.h"
#include "config.h"
#include "msgpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Text binds are SQLITE_STATIC: every bound buffer (caller-owned struct,
// caller string, literal or local array) outlives its statement, which is
// finalized before the binding function returns.

void dbUtcNow(char *out, size_t n) {
  time_t t = time(NULL);
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int dbOpen(void) {
  if (sqlite3_open(g_db_path, &g_db)) {
    configLog("sqlite open %s: %s", g_db_path, sqlite3_errmsg(g_db));
    return -1;
  }
  const char *schema = "CREATE TABLE IF NOT EXISTS raw_inputs("
                       "id INTEGER PRIMARY KEY,source TEXT,ext_id TEXT UNIQUE,"
                       "app TEXT,title TEXT,text TEXT,from_addr TEXT,received_at TEXT);"
                       "CREATE TABLE IF NOT EXISTS events("
                       "id INTEGER PRIMARY KEY,raw_id INTEGER,title TEXT,starts_at TEXT,"
                       "deadline TEXT,location TEXT,kind TEXT,confidence REAL,created_at TEXT);"
                       "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY,v TEXT);";
  char *err = NULL;
  if (sqlite3_exec(g_db, schema, NULL, NULL, &err)) {
    configLog("schema: %s", err ? err : "?");
    sqlite3_free(err);
    return -1;
  }
  return 0;
}

static void dbDjbHex(const char *a, const char *b, const char *c, char *out, size_t n) {
  unsigned long long h = 5381;
  for (const char *s = a; s && *s; s++) h = h * 33 + (unsigned char)*s;
  for (const char *s = b; s && *s; s++) h = h * 33 + (unsigned char)*s;
  for (const char *s = c; s && *s; s++) h = h * 33 + (unsigned char)*s;
  snprintf(out, n, "%016llx", h);
}

// Parse msgpack map body into Input. Unknown keys skipped. Missing ext_id is
// derived by hash of source/title/text, missing time is now. Returns 0 ok.
int dbParseInput(const unsigned char *body, long len, Input *in) {
  *in = (Input){0};
  MpReader r = {body, body + len};
  long n = mpHdrLen(&r, 0x80, 0xde, 0xdf);
  if (n < 0 || n > 64) return -1;
  for (long i = 0; i < n; i++) {
    char k[32];
    if (mpStrVal(&r, k, sizeof k)) return -1;
    char *dst = NULL;
    size_t cap = 0;
    if (!strcmp(k, "source")) {
      dst = in->source;
      cap = sizeof in->source;
    } else if (!strcmp(k, "app")) {
      dst = in->app;
      cap = sizeof in->app;
    } else if (!strcmp(k, "title")) {
      dst = in->title;
      cap = sizeof in->title;
    } else if (!strcmp(k, "text")) {
      dst = in->text;
      cap = sizeof in->text;
    } else if (!strcmp(k, "time")) {
      dst = in->time;
      cap = sizeof in->time;
    } else if (!strcmp(k, "from")) {
      dst = in->from;
      cap = sizeof in->from;
    } else if (!strcmp(k, "ext_id")) {
      dst = in->ext_id;
      cap = sizeof in->ext_id;
    }
    if (dst) {
      if (mpStrVal(&r, dst, cap)) return -1;
    } else if (mpSkip(&r)) {
      return -1;
    }
  }
  if (!in->ext_id[0]) dbDjbHex(in->source, in->title, in->text, in->ext_id, sizeof in->ext_id);
  if (!in->time[0]) dbUtcNow(in->time, sizeof in->time);
  return 0;
}

// Store raw input, dedupe by ext_id. Returns raw_id, {0} on error.
static RawId dbStoreRaw(const Input *in) {
  RawId id = {0};
  sqlite3_stmt *st = NULL;
  const char *sql = "INSERT OR IGNORE INTO raw_inputs"
                    "(source,ext_id,app,title,text,from_addr,received_at)"
                    " VALUES(?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL)) return id;
  sqlite3_bind_text(st, 1, in->source, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, in->ext_id, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 3, in->app, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 4, in->title, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 5, in->text, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 6, in->from, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 7, in->time, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) return id;
  st = NULL;
  if (sqlite3_prepare_v2(g_db, "SELECT id FROM raw_inputs WHERE ext_id=?;", -1, &st, NULL))
    return id;
  sqlite3_bind_text(st, 1, in->ext_id, -1, SQLITE_STATIC);
  if (sqlite3_step(st) == SQLITE_ROW) id.v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return id;
}

// EventKind -> wire/SQL text. Values are a contract: never rename.
static const char *const db_kind_text[] = {
    [KIND_NONE] = "none", [KIND_APPOINTMENT] = "appointment", [KIND_OBLIGATION] = "obligation"};

// Idempotent: a raw_id that already has an event is left alone. Returns 0 ok.
static int dbStoreEvent(RawId raw_id, const Classified *c) {
  char now[32];
  dbUtcNow(now, sizeof now);
  sqlite3_stmt *st = NULL;
  const char *sql = "INSERT INTO events"
                    "(raw_id,title,starts_at,deadline,location,kind,confidence,created_at)"
                    " SELECT ?1,?2,?3,?4,?5,?6,?7,?8"
                    " WHERE NOT EXISTS (SELECT 1 FROM events WHERE raw_id=?1);";
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL)) return -1;
  sqlite3_bind_int64(st, 1, raw_id.v);
  sqlite3_bind_text(st, 2, c->title, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 3, c->starts_at, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 4, c->deadline, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 5, c->location, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 6, db_kind_text[c->kind], -1, SQLITE_STATIC);
  sqlite3_bind_double(st, 7, c->confidence);
  sqlite3_bind_text(st, 8, now, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? 0 : -1;
}

Ingest dbIngest(const Input *in) {
  Ingest out = {0};
  RawId id = dbStoreRaw(in);
  if (id.v == 0) {
    configLog("store raw: %s", sqlite3_errmsg(g_db));
    return out;
  }
  Classified c = classifyInput(in);
  if (c.is_event && dbStoreEvent(id, &c)) {
    configLog("store event: %s", sqlite3_errmsg(g_db));
    return out;
  }
  out.id = id;
  out.is_event = c.is_event;
  return out;
}

// Pack all events in range as msgpack array. since/until "" = unbounded.
int dbPackEvents(const char *since, const char *until, MpWriter *w) {
  sqlite3_stmt *st = NULL;
  // COALESCE: text columns may be NULL; mpStr needs a string.
  const char *sql = "SELECT id,raw_id,COALESCE(title,''),COALESCE(starts_at,''),"
                    "COALESCE(deadline,''),COALESCE(location,''),COALESCE(kind,''),"
                    "confidence,COALESCE(created_at,'')"
                    " FROM events WHERE (?1='' OR COALESCE(NULLIF(starts_at,''),NULLIF(deadline,''),created_at)>=?1)"
                    " AND (?2='' OR COALESCE(NULLIF(starts_at,''),NULLIF(deadline,''),created_at)<=?2)"
                    " ORDER BY created_at,id;";
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL)) return -1;
  sqlite3_bind_text(st, 1, since, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 2, until, -1, SQLITE_STATIC);
  // array32 header with a count placeholder, patched after the loop, so rows
  // encode straight into w. Index, not pointer: w->p may move on realloc.
  mpReserve(w, 5);
  size_t hdr_at = w->len;
  w->p[w->len++] = 0xdd;
  w->len += 4;
  unsigned long count = 0;
  while (sqlite3_step(st) == SQLITE_ROW) {
    mpMap(w, 9);
    mpStr(w, "id");
    mpU64(w, (unsigned long long)sqlite3_column_int64(st, 0));
    mpStr(w, "raw_id");
    mpU64(w, (unsigned long long)sqlite3_column_int64(st, 1));
    mpStr(w, "title");
    mpStr(w, (const char *)sqlite3_column_text(st, 2));
    mpStr(w, "starts_at");
    mpStr(w, (const char *)sqlite3_column_text(st, 3));
    mpStr(w, "deadline");
    mpStr(w, (const char *)sqlite3_column_text(st, 4));
    mpStr(w, "location");
    mpStr(w, (const char *)sqlite3_column_text(st, 5));
    mpStr(w, "kind");
    mpStr(w, (const char *)sqlite3_column_text(st, 6));
    mpStr(w, "created_at");
    mpStr(w, (const char *)sqlite3_column_text(st, 8));
    mpStr(w, "confidence");
    mpF64(w, sqlite3_column_double(st, 7));
    count++;
  }
  sqlite3_finalize(st);
  w->p[hdr_at + 1] = (unsigned char)(count >> 24);
  w->p[hdr_at + 2] = (unsigned char)(count >> 16);
  w->p[hdr_at + 3] = (unsigned char)(count >> 8);
  w->p[hdr_at + 4] = (unsigned char)count;
  return 0;
}

ImapUid dbMetaUid(void) {
  sqlite3_stmt *st = NULL;
  ImapUid uid = {0};
  if (!sqlite3_prepare_v2(g_db, "SELECT COALESCE(v,'0') FROM meta WHERE k='imap_last_uid';",
                          -1, &st, NULL)) {
    if (sqlite3_step(st) == SQLITE_ROW) uid.v = atol((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  return uid;
}

void dbMetaUidSet(ImapUid uid) {
  char v[32];
  snprintf(v, sizeof v, "%ld", uid.v);
  sqlite3_stmt *st = NULL;
  if (!sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO meta(k,v) VALUES('imap_last_uid',?);",
                          -1, &st, NULL)) {
    sqlite3_bind_text(st, 1, v, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
}
