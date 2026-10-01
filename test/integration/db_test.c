// db module tests: ingest dedupe, event feed, range filter, meta, retry
// after a Jev failure — through db.h against a temp sqlite file, classifying
// via the fake Zen server (zen_fake.h). No real network.
#include "../../src/classify.h"
#include "../../src/common.h"
#include "../../src/config.h"
#include "../../src/db.h"
#include "../../src/msgpack.h"
#include "../vendor/greatest.h"
#include "zen_fake.h"

#include <curl/curl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmp_db_path[256] = "";

// Build Input through the boundary parser, as /ingest does. ext "" = omitted.
static int testFillInput(Input *in, const char *ext, const char *title, const char *text) {
  MpWriter w = {0};
  mpMap(&w, ext[0] ? 4 : 3);
  mpStr(&w, "source");
  mpStr(&w, "notif");
  mpStr(&w, "title");
  mpStr(&w, title);
  mpStr(&w, "text");
  mpStr(&w, text);
  if (ext[0]) {
    mpStr(&w, "ext_id");
    mpStr(&w, ext);
  }
  int rc = dbParseInput(w.p, (i64)w.len, in);
  free(w.p);
  return rc;
}

// Events in range; with title != "" only those with exactly that title.
// -1 on error.
static i64 testFeedCount(const char *since, const char *until, const char *title) {
  MpWriter w = {0};
  if (dbPackEvents(since, until, &w)) return -1;
  MpReader r = {w.p, w.p + w.len};
  i64 n = mpHdrLen(&r, 0x90, 0xdc, 0xdd);
  int bad = n < 0;
  i64 count = 0;
  for (i64 i = 0; i < n && !bad; i++) {
    i64 m = mpHdrLen(&r, 0x80, 0xde, 0xdf);
    bad = m < 0;
    int match = !title[0];
    for (i64 j = 0; j < m && !bad; j++) {
      char k[32], v[512]; // every event value is a scalar
      bad = mpStrVal(&r, k, sizeof k) || mpStrVal(&r, v, sizeof v);
      if (!bad && !strcmp(k, "title") && !strcmp(v, title)) match = 1;
    }
    count += match;
  }
  free(w.p);
  return bad ? -1 : count;
}

// One integer from a query on the open db; -1 if no row. raw_inputs has no
// read API, and its schema is a contract (AGENTS.md), so peek it.
static i64 testDbI64(const char *sql) {
  sqlite3_stmt *st = NULL;
  i64 v = -1;
  if (!sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW)
    v = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return v;
}

static void testSetup(void *arg) {
  (void)arg;
  zenFakeMode(ZEN_FAKE_OK);
}

// The fake Jev answers none for these: raw-only, no events rows.
TEST storesAndDedupesRaw(void) {
  Input in;
  ASSERT_EQ(0, testFillInput(&in, "db-raw-1", "Note to self", "confirming"));
  Ingest first = dbIngest(&in);
  ASSERT(first.id.v > 0);
  ASSERT_EQ(0, first.is_event);
  ASSERT_EQ(first.id.v, dbIngest(&in).id.v);
  PASS();
}

TEST derivesMissingExtId(void) {
  Input a, b;
  ASSERT_EQ(0, testFillInput(&a, "", "Note to self", "unique derive body"));
  ASSERT_EQ(0, testFillInput(&b, "", "Note to self", "unique derive body"));
  ASSERT(a.time[0]); // missing time defaults to now
  RawId first = dbIngest(&a).id;
  ASSERT(first.v > 0);
  ASSERT_EQ(first.v, dbIngest(&b).id.v); // same content, no ext_id: dedupes
  ASSERT_EQ(0, testFillInput(&b, "", "Note to self", "another body"));
  ASSERT(dbIngest(&b).id.v != first.v); // different content: distinct row
  PASS();
}

TEST storesAndFiltersEvents(void) {
  const char *mid = "2026-10-15T00:00:00Z";
  i64 all = testFeedCount("", "", "");
  i64 late = testFeedCount(mid, "", "");
  i64 early = testFeedCount("", mid, "");
  Input in;
  ASSERT_EQ(0, testFillInput(&in, "db-ev-a", "Dentist 2026-10-01T10:00", "confirming"));
  ASSERT(dbIngest(&in).is_event);
  ASSERT_EQ(0, testFillInput(&in, "db-ev-b", "Dentist 2026-11-01T10:00", "confirming"));
  ASSERT(dbIngest(&in).is_event);
  ASSERT_EQ(all + 2, testFeedCount("", "", ""));
  ASSERT_EQ(late + 1, testFeedCount(mid, "", ""));
  ASSERT_EQ(early + 1, testFeedCount("", mid, ""));
  ASSERT_EQ(1, testFeedCount("", "", "Dentist 2026-10-01T10:00"));
  ASSERT_EQ(1, testFeedCount(mid, "", "Dentist 2026-11-01T10:00"));
  ASSERT_EQ(0, testFeedCount(mid, "", "Dentist 2026-10-01T10:00"));
  ASSERT_EQ(0, testFeedCount("", mid, "Dentist 2026-11-01T10:00"));
  PASS();
}

TEST ingestTwiceStoresOneEvent(void) {
  Input in;
  ASSERT_EQ(0, testFillInput(&in, "db-ev-twice", "Dentist twice", "confirming"));
  i64 before = testFeedCount("", "", "");
  Ingest first = dbIngest(&in);
  Ingest again = dbIngest(&in); // e.g. same mail seen by IMAP again
  ASSERT(first.id.v > 0);
  ASSERT_EQ(first.id.v, again.id.v);
  ASSERT(first.is_event && again.is_event);
  ASSERT_EQ(before + 1, testFeedCount("", "", ""));
  ASSERT_EQ(1, testFeedCount("", "", "Dentist twice"));
  PASS();
}

TEST jevFailureKeepsRawAndRetries(void) {
  Input in;
  ASSERT_EQ(0, testFillInput(&in, "db-retry-1", "Dentist retry 2026-12-01T10:00", "confirming"));
  zenFakeMode(ZEN_FAKE_FAIL_500);
  ASSERT_EQ(0, dbIngest(&in).id.v);
  i64 kept = testDbI64("SELECT id FROM raw_inputs WHERE ext_id='db-retry-1';");
  ASSERT(kept > 0); // raw stored before classifying
  ASSERT_EQ(0, testFeedCount("", "", "Dentist retry 2026-12-01T10:00"));
  zenFakeMode(ZEN_FAKE_OK);
  Ingest retry = dbIngest(&in);
  ASSERT_EQ(kept, retry.id.v);
  ASSERT(retry.is_event);
  ASSERT_EQ(kept, dbIngest(&in).id.v);
  ASSERT_EQ(1, testFeedCount("", "", "Dentist retry 2026-12-01T10:00"));
  ASSERT_EQ(1, testDbI64("SELECT COUNT(*) FROM raw_inputs WHERE ext_id='db-retry-1';"));
  PASS();
}

TEST metaRoundTrip(void) {
  dbMetaUidSet((ImapUid){42});
  ASSERT_EQ(42, dbMetaUid().v);
  dbMetaUidSet((ImapUid){7}); // overwrites, even downward
  ASSERT_EQ(7, dbMetaUid().v);
  PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  curl_global_init(CURL_GLOBAL_ALL);
  if (zenFakeStart() || classifyInit()) {
    fprintf(stderr, "classify setup failed\n");
    return 1;
  }
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
    sqlite3_close(g_db);
    unlink(tmpl);
    return 1;
  }
  snprintf(tmp_db_path, sizeof tmp_db_path, "%s", tmpl);
  SET_SETUP(testSetup, NULL);
  RUN_TEST(storesAndDedupesRaw);
  RUN_TEST(derivesMissingExtId);
  RUN_TEST(storesAndFiltersEvents);
  RUN_TEST(ingestTwiceStoresOneEvent);
  RUN_TEST(jevFailureKeepsRawAndRetries);
  RUN_TEST(metaRoundTrip);
  sqlite3_close(g_db);
  unlink(tmp_db_path);
  GREATEST_MAIN_END();
  (void)argc;
  (void)argv;
}
