// SQLite persistence: raw inputs, classified events, IMAP progress.
#include "db.h"

#include "common.h"
#include "msgpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void utc_now(char *out, size_t n) {
  time_t t = time(NULL);
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int db_open(void) {
  if (sqlite3_open(g_db_path, &g_db)) {
    log_msg("sqlite open %s: %s", g_db_path, sqlite3_errmsg(g_db));
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
    log_msg("schema: %s", err ? err : "?");
    sqlite3_free(err);
    return -1;
  }
  return 0;
}

static void djb_hex(const char *a, const char *b, const char *c, char *out, size_t n) {
  unsigned long long h = 5381;
  for (const char *s = a; s && *s; s++) h = h * 33 + (unsigned char)*s;
  for (const char *s = b; s && *s; s++) h = h * 33 + (unsigned char)*s;
  for (const char *s = c; s && *s; s++) h = h * 33 + (unsigned char)*s;
  snprintf(out, n, "%016llx", h);
}

// Parse msgpack map body into Input. Unknown keys skipped. Returns 0 ok.
int parse_input(const unsigned char *body, long len, Input *in) {
  memset(in, 0, sizeof *in);
  MPR r = {body, body + len};
  long n = mp_hdr_len(&r, 0x80, 0xde, 0xdf);
  if (n < 0 || n > 64) return -1;
  for (long i = 0; i < n; i++) {
    char *k = mp_strval(&r);
    if (!k) return -1;
    char *dst = NULL;
    size_t cap = 0;
    if (!strcmp(k, "source"))
      dst = in->source, cap = sizeof in->source;
    else if (!strcmp(k, "app"))
      dst = in->app, cap = sizeof in->app;
    else if (!strcmp(k, "title"))
      dst = in->title, cap = sizeof in->title;
    else if (!strcmp(k, "text"))
      dst = in->text, cap = sizeof in->text;
    else if (!strcmp(k, "time"))
      dst = in->time, cap = sizeof in->time;
    else if (!strcmp(k, "from"))
      dst = in->from, cap = sizeof in->from;
    else if (!strcmp(k, "ext_id"))
      dst = in->ext_id, cap = sizeof in->ext_id;
    if (dst) {
      char *v = mp_strval(&r);
      if (!v) {
        free(k);
        return -1;
      }
      snprintf(dst, cap, "%s", v);
      free(v);
    } else if (mp_skip(&r)) {
      free(k);
      return -1;
    }
    free(k);
  }
  if (!in->ext_id[0]) djb_hex(in->source, in->title, in->text, in->ext_id, sizeof in->ext_id);
  return 0;
}

// Store raw input, dedupe by ext_id. Returns raw_id (>0) or -1 on error.
long long store_raw(const Input *in) {
  char now[32];
  utc_now(now, sizeof now);
  sqlite3_stmt *st = NULL;
  const char *sql = "INSERT OR IGNORE INTO raw_inputs"
                    "(source,ext_id,app,title,text,from_addr,received_at)"
                    " VALUES(?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL)) return -1;
  sqlite3_bind_text(st, 1, in->source, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, in->ext_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, in->app, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, in->title, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, in->text, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, in->from, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, in->time[0] ? in->time : now, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) return -1;
  st = NULL;
  if (sqlite3_prepare_v2(g_db, "SELECT id FROM raw_inputs WHERE ext_id=?;", -1, &st, NULL))
    return -1;
  sqlite3_bind_text(st, 1, in->ext_id, -1, SQLITE_TRANSIENT);
  long long id = -1;
  if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return id;
}

int store_event(long long raw_id, const Classified *c) {
  char now[32];
  utc_now(now, sizeof now);
  sqlite3_stmt *st = NULL;
  const char *sql = "INSERT INTO events"
                    "(raw_id,title,starts_at,deadline,location,kind,confidence,created_at)"
                    " VALUES(?,?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL)) return -1;
  sqlite3_bind_int64(st, 1, raw_id);
  sqlite3_bind_text(st, 2, c->title, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, c->starts_at, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, c->deadline, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, c->location, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, c->kind, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(st, 7, c->confidence);
  sqlite3_bind_text(st, 8, now, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? 0 : -1;
}

// Pack all events in range as msgpack array. since/until "" = unbounded.
int pack_events(const char *since, const char *until, MPW *w) {
  sqlite3_stmt *st = NULL;
  const char *sql = "SELECT id,raw_id,title,starts_at,deadline,location,kind,confidence,created_at"
                    " FROM events WHERE (?1='' OR COALESCE(NULLIF(starts_at,''),NULLIF(deadline,''),created_at)>=?1)"
                    " AND (?2='' OR COALESCE(NULLIF(starts_at,''),NULLIF(deadline,''),created_at)<=?2)"
                    " ORDER BY created_at,id;";
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL)) return -1;
  sqlite3_bind_text(st, 1, since, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, until, -1, SQLITE_TRANSIENT);
  MPW items = {0};
  unsigned long count = 0;
  const char *keys[7] = {"title", "starts_at", "deadline", "location", "kind", "", "created_at"};
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *cols[7] = {(const char *)sqlite3_column_text(st, 2),
                           (const char *)sqlite3_column_text(st, 3),
                           (const char *)sqlite3_column_text(st, 4),
                           (const char *)sqlite3_column_text(st, 5),
                           (const char *)sqlite3_column_text(st, 6),
                           NULL,
                           (const char *)sqlite3_column_text(st, 8)};
    mp_map(&items, 9);
    mp_str(&items, "id");
    mp_u64(&items, (unsigned long long)sqlite3_column_int64(st, 0));
    mp_str(&items, "raw_id");
    mp_u64(&items, (unsigned long long)sqlite3_column_int64(st, 1));
    for (int i = 0; i < 7; i++) {
      if (i == 5) continue;
      mp_str(&items, keys[i]);
      mp_str(&items, cols[i] ? cols[i] : "");
    }
    mp_str(&items, "confidence");
    mp_f64(&items, sqlite3_column_double(st, 7));
    count++;
  }
  sqlite3_finalize(st);
  mp_arr(w, count);
  if (count) {
    mp_reserve(w, items.len);
    memcpy(w->p + w->len, items.p, items.len);
    w->len += items.len;
  }
  free(items.p);
  return 0;
}

long meta_uid(void) {
  sqlite3_stmt *st = NULL;
  long uid = 0;
  if (!sqlite3_prepare_v2(g_db, "SELECT v FROM meta WHERE k='imap_last_uid';", -1, &st, NULL)) {
    if (sqlite3_step(st) == SQLITE_ROW) uid = atol((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
  }
  return uid;
}

void meta_uid_set(long uid) {
  char v[32];
  snprintf(v, sizeof v, "%ld", uid);
  sqlite3_stmt *st = NULL;
  if (!sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO meta(k,v) VALUES('imap_last_uid',?);",
                          -1, &st, NULL)) {
    sqlite3_bind_text(st, 1, v, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
}
