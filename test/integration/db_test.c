// db module tests: raw dedupe, event feed, range filter, meta — through db.h
// against a temp sqlite file. No server, no network.
#include "../../src/common.h"
#include "../../src/config.h"
#include "../../src/db.h"
#include "../../src/msgpack.h"
#include "../vendor/greatest.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tDb[256] = "";

static void fillInput(Input *in, const char *ext) {
  memset(in, 0, sizeof *in);
  snprintf(in->source, sizeof in->source, "notif");
  snprintf(in->title, sizeof in->title, "Dentist visit");
  snprintf(in->text, sizeof in->text, "confirming");
  snprintf(in->ext_id, sizeof in->ext_id, "%s", ext);
}

static long feedCount(const char *since, const char *until) {
  MpWriter w = {0};
  if (dbPackEvents(since, until, &w)) return -1;
  MpReader r = {w.p, w.p + w.len};
  long n = mpHdrLen(&r, 0x90, 0xdc, 0xdd);
  free(w.p);
  return n;
}

// 1 if any event in range has this title, 0 if not, -1 on error.
static int feedHas(const char *since, const char *until, const char *t) {
  MpWriter w = {0};
  if (dbPackEvents(since, until, &w)) return -1;
  MpReader r = {w.p, w.p + w.len};
  long n = mpHdrLen(&r, 0x90, 0xdc, 0xdd);
  int found = 0;
  for (long i = 0; i < n && !found; i++) {
    long m = mpHdrLen(&r, 0x80, 0xde, 0xdf);
    if (m < 0) {
      free(w.p);
      return -1;
    }
    for (long j = 0; j < m; j++) {
      char *k = mpStrval(&r);
      if (!k) {
        free(w.p);
        return -1;
      }
      if (!strcmp(k, "title")) {
        char *v = mpStrval(&r);
        found = v && !strcmp(v, t);
        free(v);
      } else if (mpSkip(&r)) {
        free(k);
        free(w.p);
        return -1;
      }
      free(k);
    }
  }
  free(w.p);
  return found;
}

TEST storesAndDedupesRaw(void) {
  Input in;
  fillInput(&in, "db-raw-1");
  long long first = dbStoreRaw(&in);
  ASSERT(first > 0);
  ASSERT_EQ(first, dbStoreRaw(&in));
  PASS();
}

TEST derivesMissingExtId(void) {
  Input a, b;
  fillInput(&a, "");
  a.ext_id[0] = 0;
  snprintf(a.text, sizeof a.text, "unique derive body");
  fillInput(&b, "");
  b.ext_id[0] = 0;
  snprintf(b.text, sizeof b.text, "unique derive body");
  long long first = dbStoreRaw(&a);
  ASSERT(first > 0);
  ASSERT_EQ(first, dbStoreRaw(&b)); // same content, no ext_id: dedupes
  fillInput(&b, "");
  b.ext_id[0] = 0;
  snprintf(b.text, sizeof b.text, "different body");
  ASSERT(dbStoreRaw(&b) != first); // different content: distinct row
  PASS();
}

TEST storesAndFiltersEvents(void) {
  Input in;
  fillInput(&in, "db-ev-a");
  long long ra = dbStoreRaw(&in);
  fillInput(&in, "db-ev-b");
  long long rb = dbStoreRaw(&in);
  Classified a, b;
  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  snprintf(a.title, sizeof a.title, "October visit");
  snprintf(a.starts_at, sizeof a.starts_at, "2026-10-01T10:00:00Z");
  snprintf(a.kind, sizeof a.kind, "appointment");
  a.confidence = 0.9;
  snprintf(b.title, sizeof b.title, "November visit");
  snprintf(b.starts_at, sizeof b.starts_at, "2026-11-01T10:00:00Z");
  snprintf(b.kind, sizeof b.kind, "appointment");
  b.confidence = 0.8;
  ASSERT_EQ(0, dbStoreEvent(ra, &a));
  ASSERT_EQ(0, dbStoreEvent(rb, &b));
  ASSERT_EQ(2, feedCount("", ""));
  ASSERT_EQ(1, feedCount("2026-10-15T00:00:00Z", ""));
  ASSERT_EQ(1, feedCount("", "2026-10-15T00:00:00Z"));
  ASSERT_EQ(1, feedHas("", "", "October visit"));
  ASSERT_EQ(1, feedHas("2026-10-15T00:00:00Z", "", "November visit"));
  ASSERT_EQ(0, feedHas("2026-10-15T00:00:00Z", "", "October visit"));
  PASS();
}

TEST storeEventIsIdempotent(void) {
  Input in;
  fillInput(&in, "db-ev-twice");
  long long raw = dbStoreRaw(&in);
  ASSERT(raw > 0);
  Classified c;
  memset(&c, 0, sizeof c);
  snprintf(c.title, sizeof c.title, "Twice stored visit");
  snprintf(c.kind, sizeof c.kind, "appointment");
  c.confidence = 0.9;
  long before = feedCount("", "");
  ASSERT_EQ(0, dbStoreEvent(raw, &c));
  ASSERT_EQ(0, dbStoreEvent(raw, &c)); // e.g. same mail seen by IMAP again
  ASSERT_EQ(before + 1, feedCount("", ""));
  PASS();
}

TEST metaRoundTrip(void) {
  ASSERT_EQ(0, dbMetaUid());
  dbMetaUidSet(42);
  ASSERT_EQ(42, dbMetaUid());
  PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  char tmpl[] = "/tmp/genda-db-XXXXXX.db";
  int tfd = mkstemps(tmpl, 3);
  if (tfd < 0) {
    fprintf(stderr, "db setup failed\n");
    return 1;
  }
  close(tfd);
  snprintf(g_db_path, sizeof g_db_path, "%s", tmpl);
  if (dbOpen()) {
    fprintf(stderr, "db setup failed\n");
    return 1;
  }
  snprintf(tDb, sizeof tDb, "%s", tmpl);
  RUN_TEST(storesAndDedupesRaw);
  RUN_TEST(derivesMissingExtId);
  RUN_TEST(storesAndFiltersEvents);
  RUN_TEST(storeEventIsIdempotent);
  RUN_TEST(metaRoundTrip);
  GREATEST_MAIN_END();
  sqlite3_close(g_db);
  unlink(tDb);
  return 0;
  (void)argc;
  (void)argv;
}
