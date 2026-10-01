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

static char tDb[256] = "";

// Build Input through the boundary parser, as /ingest does. ext "" = omitted.
static int fillInput(Input *in, const char *ext, const char *title, const char *text) {
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
      char k[32], v[512];
      if (mpStrval(&r, k, sizeof k)) {
        free(w.p);
        return -1;
      }
      if (!strcmp(k, "title")) {
        found = !mpStrval(&r, v, sizeof v) && !strcmp(v, t);
      } else if (mpSkip(&r)) {
        free(w.p);
        return -1;
      }
    }
  }
  free(w.p);
  return found;
}

// Titles without event keywords keep these raw-only: no events rows.
TEST storesAndDedupesRaw(void) {
  Input in;
  ASSERT_EQ(0, fillInput(&in, "db-raw-1", "Note to self", "confirming"));
  Ingest first = dbIngest(&in);
  ASSERT(first.id.v > 0);
  ASSERT_EQ(0, first.is_event);
  ASSERT_EQ(first.id.v, dbIngest(&in).id.v);
  PASS();
}

TEST derivesMissingExtId(void) {
  Input a, b;
  ASSERT_EQ(0, fillInput(&a, "", "Note to self", "unique derive body"));
  ASSERT_EQ(0, fillInput(&b, "", "Note to self", "unique derive body"));
  ASSERT(a.time[0]); // missing time defaults to now
  RawId first = dbIngest(&a).id;
  ASSERT(first.v > 0);
  ASSERT_EQ(first.v, dbIngest(&b).id.v); // same content, no ext_id: dedupes
  ASSERT_EQ(0, fillInput(&b, "", "Note to self", "another body"));
  ASSERT(dbIngest(&b).id.v != first.v); // different content: distinct row
  PASS();
}

TEST storesAndFiltersEvents(void) {
  Input in;
  ASSERT_EQ(0, fillInput(&in, "db-ev-a", "Dentist 2026-10-01T10:00", "confirming"));
  ASSERT(dbIngest(&in).is_event);
  ASSERT_EQ(0, fillInput(&in, "db-ev-b", "Dentist 2026-11-01T10:00", "confirming"));
  ASSERT(dbIngest(&in).is_event);
  ASSERT_EQ(2, feedCount("", ""));
  ASSERT_EQ(1, feedCount("2026-10-15T00:00:00Z", ""));
  ASSERT_EQ(1, feedCount("", "2026-10-15T00:00:00Z"));
  ASSERT_EQ(1, feedHas("", "", "Dentist 2026-10-01T10:00"));
  ASSERT_EQ(1, feedHas("2026-10-15T00:00:00Z", "", "Dentist 2026-11-01T10:00"));
  ASSERT_EQ(0, feedHas("2026-10-15T00:00:00Z", "", "Dentist 2026-10-01T10:00"));
  PASS();
}

TEST ingestTwiceStoresOneEvent(void) {
  Input in;
  ASSERT_EQ(0, fillInput(&in, "db-ev-twice", "Dentist twice", "confirming"));
  long before = feedCount("", "");
  Ingest first = dbIngest(&in);
  Ingest again = dbIngest(&in); // e.g. same mail seen by IMAP again
  ASSERT(first.id.v > 0);
  ASSERT_EQ(first.id.v, again.id.v);
  ASSERT(first.is_event && again.is_event);
  ASSERT_EQ(before + 1, feedCount("", ""));
  PASS();
}

TEST metaRoundTrip(void) {
  ASSERT_EQ(0, dbMetaUid().v);
  dbMetaUidSet((ImapUid){42});
  ASSERT_EQ(42, dbMetaUid().v);
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
  snprintf(tDb, sizeof tDb, "%s", tmpl);
  RUN_TEST(storesAndDedupesRaw);
  RUN_TEST(derivesMissingExtId);
  RUN_TEST(storesAndFiltersEvents);
  RUN_TEST(ingestTwiceStoresOneEvent);
  RUN_TEST(metaRoundTrip);
  sqlite3_close(g_db);
  unlink(tDb);
  GREATEST_MAIN_END();
  (void)argc;
  (void)argv;
}
