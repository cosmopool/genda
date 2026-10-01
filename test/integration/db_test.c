// db module tests: ingest dedupe, event feed, range filter, meta — through
// db.h against a temp sqlite file. No server, no network.
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
  int rc = dbParseInput(w.p, (long)w.len, in);
  free(w.p);
  return rc;
}

// Events in range; with title != "" only those with exactly that title.
// -1 on error.
static long testFeedCount(const char *since, const char *until, const char *title) {
  MpWriter w = {0};
  if (dbPackEvents(since, until, &w)) return -1;
  MpReader r = {w.p, w.p + w.len};
  long n = mpHdrLen(&r, 0x90, 0xdc, 0xdd);
  int bad = n < 0;
  long count = 0;
  for (long i = 0; i < n && !bad; i++) {
    long m = mpHdrLen(&r, 0x80, 0xde, 0xdf);
    bad = m < 0;
    int match = !title[0];
    for (long j = 0; j < m && !bad; j++) {
      char k[32], v[512]; // every event value is a scalar
      bad = mpStrVal(&r, k, sizeof k) || mpStrVal(&r, v, sizeof v);
      if (!bad && !strcmp(k, "title") && !strcmp(v, title)) match = 1;
    }
    count += match;
  }
  free(w.p);
  return bad ? -1 : count;
}

// Titles without event keywords keep these raw-only: no events rows.
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
  long all = testFeedCount("", "", "");
  long late = testFeedCount(mid, "", "");
  long early = testFeedCount("", mid, "");
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
  long before = testFeedCount("", "", "");
  Ingest first = dbIngest(&in);
  Ingest again = dbIngest(&in); // e.g. same mail seen by IMAP again
  ASSERT(first.id.v > 0);
  ASSERT_EQ(first.id.v, again.id.v);
  ASSERT(first.is_event && again.is_event);
  ASSERT_EQ(before + 1, testFeedCount("", "", ""));
  ASSERT_EQ(1, testFeedCount("", "", "Dentist twice"));
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
  RUN_TEST(storesAndDedupesRaw);
  RUN_TEST(derivesMissingExtId);
  RUN_TEST(storesAndFiltersEvents);
  RUN_TEST(ingestTwiceStoresOneEvent);
  RUN_TEST(metaRoundTrip);
  sqlite3_close(g_db);
  unlink(tmp_db_path);
  GREATEST_MAIN_END();
  (void)argc;
  (void)argv;
}
