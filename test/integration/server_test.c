// Integration tests: real genda binary + temp sqlite, exercised over HTTP.
// Asserts on wire behavior (msgpack): events only via GET /events. genda
// classifies against the fake Zen server (zen_fake.h) in this process.
#include <arpa/inet.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../src/core.h"
#include "../vendor/greatest.h"
#include "zen_fake.h"

#define TOKEN "t-secret"

static char g_db_path[256] = "";
static pid_t g_child = -1;
static i32 test_port = 0;

// ---- tiny http client (test side) ---------------------------------------

typedef struct {
  int status;
  u8 *body;
  i64 len;
} Resp;

static void testRespFree(Resp *r) {
  free(r->body);
  r->body = NULL;
}

// Send a raw request head (+ optional body) and read the whole response.
static int testHraw(const char *hdr, i64 hlen, const u8 *body, i64 blen, Resp *out) {
  memset(out, 0, sizeof *out);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval tv = {.tv_sec = 5};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((u16)test_port)};
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof a)) {
    close(fd);
    return -1;
  }
  // (headers + optional body, always fully written)
  usize off = 0;
  while (off < (usize)hlen) {
    ssize_t n = send(fd, hdr + off, (usize)hlen - off, 0);
    if (n <= 0) {
      close(fd);
      return -1;
    }
    off += (usize)n;
  }
  off = 0;
  while (body && off < (usize)blen) {
    ssize_t n = send(fd, body + off, (usize)blen - off, 0);
    if (n <= 0) {
      close(fd);
      return -1;
    }
    off += (usize)n;
  }
  usize cap = 65536, got = 0;
  u8 *buf = malloc(cap);
  if (!buf) {
    close(fd);
    return -1;
  }
  ssize_t n;
  while ((n = recv(fd, buf + got, cap - got, 0)) > 0) {
    got += (usize)n;
    if (got == cap) {
      cap *= 2;
      u8 *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        close(fd);
        return -1;
      }
      buf = nb;
    }
  }
  close(fd);
  if (sscanf((char *)buf, "HTTP/1.1 %d", &out->status) != 1) {
    free(buf);
    return -1;
  }
  char *eoh = strstr((char *)buf, "\r\n\r\n");
  if (!eoh) {
    free(buf);
    return -1;
  }
  i64 h = (i64)(eoh - (char *)buf) + 4;
  out->len = (i64)got - h;
  out->body = malloc((usize)out->len + 1);
  if (!out->body) {
    free(buf);
    return -1;
  }
  memcpy(out->body, buf + h, (usize)out->len);
  out->body[out->len] = 0;
  free(buf);
  return 0;
}

static int testHreq(const char *method, const char *path, int auth, const u8 *body,
                i64 blen, Resp *out) {
  char hdr[1024];
  int hlen = snprintf(hdr, sizeof hdr, "%s %s HTTP/1.1\r\nHost: x\r\n%sConnection: close\r\n\r\n",
                      method, path, auth ? "Authorization: Bearer " TOKEN "\r\n" : "");
  if (body)
    hlen = snprintf(hdr, sizeof hdr,
                    "%s %s HTTP/1.1\r\nHost: x\r\n%sContent-Type: application/msgpack\r\n"
                    "Content-Length: %" PRId64 "\r\nConnection: close\r\n\r\n",
                    method, path, auth ? "Authorization: Bearer " TOKEN "\r\n" : "", blen);
  return testHraw(hdr, hlen, body, blen, out);
}

// ---- tiny msgpack (test side, independent of server impl) ----------------

typedef struct {
  u8 *p;
  usize len, cap;
} Buf;

static void testBput(Buf *b, const void *s, usize n) {
  if (b->len + n > b->cap) {
    b->cap = b->cap ? b->cap * 2 : 256;
    while (b->cap < b->len + n) b->cap *= 2;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
}

static void testPkMap(Buf *b, u32 n) {
  u8 h = (u8)(0x80 | n);
  testBput(b, &h, 1);
}

static void testPkStr(Buf *b, const char *s) {
  usize n = strlen(s);
  if (n < 32) {
    u8 h = (u8)(0xa0 | n);
    testBput(b, &h, 1);
  } else {
    u8 h[2] = {0xd9, (u8)n};
    testBput(b, h, 2);
  }
  testBput(b, s, n);
}

// pack {k0:v0, ...} from parallel arrays
static void testPkInput(Buf *b, const char *keys[], const char *vals[], i32 n) {
  testPkMap(b, (u32)n);
  for (i32 i = 0; i < n; i++) {
    testPkStr(b, keys[i]);
    testPkStr(b, vals[i]);
  }
}

typedef enum { N_NIL, N_BOOL, N_UINT, N_STR, N_ARR, N_MAP, N_DBL } NodeType;
typedef struct Node {
  NodeType t;
  u64 u;
  f64 d;
  char *s;
  struct Node **items;
  u64 n; // arr len, or map pair count (*2 items)
} Node;

typedef struct {
  const u8 *p, *end;
} Cur;

static void testNfree(Node *nd) {
  if (!nd) return;
  free(nd->s);
  for (u64 i = 0; i < nd->n; i++) testNfree(nd->items[i]);
  free(nd->items);
  free(nd);
}

static Node *testParse(Cur *c);

static u64 testBeU(Cur *c, i32 n) {
  u64 v = 0;
  for (i32 i = 0; i < n; i++) v = (v << 8) | c->p[i];
  c->p += n;
  return v;
}

static Node *testParse(Cur *c) {
  if (c->p >= c->end) return NULL;
  u8 b = *c->p++;
  Node *nd = calloc(1, sizeof *nd);
  if (!nd) return NULL;
  if ((b & 0xf0) == 0x80 || b == 0xde || b == 0xdf) {
    u64 n = (b & 0xf0) == 0x80 ? (u32)(b & 0x0f)
                               : b == 0xde ? (u32)testBeU(c, 2) : (u32)testBeU(c, 4);
    nd->t = N_MAP;
    nd->n = n * 2;
    nd->items = calloc(nd->n ? nd->n : 1, sizeof *nd->items);
    for (u64 i = 0; i < nd->n; i++)
      if (!(nd->items[i] = testParse(c))) {
        testNfree(nd);
        return NULL;
      }
    return nd;
  }
  if ((b & 0xf0) == 0x90 || b == 0xdc || b == 0xdd) {
    u64 n = (b & 0xf0) == 0x90 ? (u32)(b & 0x0f)
                               : b == 0xdc ? (u32)testBeU(c, 2) : (u32)testBeU(c, 4);
    nd->t = N_ARR;
    nd->n = n;
    nd->items = calloc(n ? n : 1, sizeof *nd->items);
    for (u64 i = 0; i < n; i++)
      if (!(nd->items[i] = testParse(c))) {
        testNfree(nd);
        return NULL;
      }
    return nd;
  }
  if ((b & 0xe0) == 0xa0 || b == 0xd9 || b == 0xda) {
    u64 n = (b & 0xe0) == 0xa0 ? (u32)(b & 0x1f)
                               : b == 0xd9 ? (u32)testBeU(c, 1) : (u32)testBeU(c, 2);
    if (c->end - c->p < (i64)n) {
      testNfree(nd);
      return NULL;
    }
    nd->t = N_STR;
    nd->s = malloc(n + 1);
    memcpy(nd->s, c->p, n);
    nd->s[n] = 0;
    c->p += n;
    return nd;
  }
  if (b < 0x80) {
    nd->t = N_UINT;
    nd->u = b;
    return nd;
  }
  if (b == 0xcf) {
    nd->t = N_UINT;
    nd->u = testBeU(c, 8);
    return nd;
  }
  if (b == 0xcb) {
    u64 u = testBeU(c, 8);
    nd->t = N_DBL;
    memcpy(&nd->d, &u, 8);
    return nd;
  }
  if (b == 0xc0) {
    nd->t = N_NIL;
    return nd;
  }
  if (b == 0xc2 || b == 0xc3) {
    nd->t = N_BOOL;
    nd->u = b == 0xc3;
    return nd;
  }
  testNfree(nd);
  return NULL;
}

static Node *testMget(Node *map, const char *key) {
  if (!map || map->t != N_MAP) return NULL;
  for (u64 i = 0; i < map->n; i += 2)
    if (map->items[i]->t == N_STR && !strcmp(map->items[i]->s, key)) return map->items[i + 1];
  return NULL;
}

static Node *testFindByTitle(Node *arr, const char *title) {
  if (!arr || arr->t != N_ARR) return NULL;
  for (u64 i = 0; i < arr->n; i++) {
    Node *t = testMget(arr->items[i], "title");
    if (t && t->t == N_STR && !strcmp(t->s, title)) return arr->items[i];
  }
  return NULL;
}

// GET /events and count events with exactly this title. -1 on error.
static i64 testEventsTitled(const char *title) {
  Resp r;
  i64 n = -1;
  if (!testHreq("GET", "/events", 1, NULL, 0, &r) && r.status == 200) {
    Cur c = {r.body, r.body + r.len};
    Node *arr = testParse(&c);
    if (arr && arr->t == N_ARR) {
      n = 0;
      for (u64 i = 0; i < arr->n; i++) {
        Node *t = testMget(arr->items[i], "title");
        if (t && t->t == N_STR && !strcmp(t->s, title)) n++;
      }
    }
    testNfree(arr);
  }
  testRespFree(&r);
  return n;
}

// ---- db peek (resulting state) -------------------------------------------

// One integer from a query; -1 if no row. raw_inputs has no API; its schema
// is a contract (AGENTS.md), so peek it.
static i64 testDbI64(const char *sql) {
  sqlite3 *db = NULL;
  if (sqlite3_open(g_db_path, &db)) return -1;
  sqlite3_stmt *st = NULL;
  i64 n = -1;
  if (!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW)
    n = (i64)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  sqlite3_close(db);
  return n;
}

// ---- cases ----------------------------------------------------------------

TEST authIsEnforced(void) {
  Resp r;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 0, (u8 *)"x", 1, &r));
  ASSERT_EQ(401, r.status);
  testRespFree(&r);
  ASSERT_EQ(0, testHreq("GET", "/events", 0, NULL, 0, &r));
  ASSERT_EQ(401, r.status);
  testRespFree(&r);
  PASS();
}

TEST notificationBecomesAppointment(void) {
  Buf b = {0};
  const char *k[] = {"source", "app", "title", "text", "time", "from", "ext_id"};
  const char *v[] = {"notif", "Gmail", "Dentist 2026-10-01T10:00", "confirming your visit",
                     "2026-09-30T09:00:00Z", "dentist@x.com", "t-appt-1"};
  testPkInput(&b, k, v, 7);
  Resp r;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (i64)b.len, &r));
  ASSERT_EQ(200, r.status);
  Cur c = {r.body, r.body + r.len};
  Node *m = testParse(&c);
  ASSERT(m);
  Node *ev = testMget(m, "is_event");
  ASSERT(ev && ev->t == N_UINT && ev->u == 1);
  testNfree(m);
  testRespFree(&r);
  free(b.p);

  ASSERT_EQ(0, testHreq("GET", "/events", 1, NULL, 0, &r));
  ASSERT_EQ(200, r.status);
  Cur c2 = {r.body, r.body + r.len};
  Node *arr = testParse(&c2);
  ASSERT(arr && arr->t == N_ARR);
  Node *found = testFindByTitle(arr, "Dentist 2026-10-01T10:00");
  ASSERT(found);
  Node *kind = testMget(found, "kind");
  ASSERT(kind && kind->t == N_STR);
  ASSERT_STR_EQ("appointment", kind->s);
  testNfree(arr);
  testRespFree(&r);
  PASS();
}

TEST emailBecomesObligation(void) {
  Buf b = {0};
  const char *k[] = {"source", "app", "title", "text", "time", "from", "ext_id"};
  const char *v[] = {"email", "billing@power.com", "Invoice due 2026-10-05", "please pay by Friday",
                     "2026-09-30T08:00:00Z", "billing@power.com", "t-oblg-1"};
  testPkInput(&b, k, v, 7);
  Resp r;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (i64)b.len, &r));
  ASSERT_EQ(200, r.status);
  testRespFree(&r);
  free(b.p);

  ASSERT_EQ(0, testHreq("GET", "/events", 1, NULL, 0, &r));
  Cur c = {r.body, r.body + r.len};
  Node *arr = testParse(&c);
  Node *found = testFindByTitle(arr, "Invoice due 2026-10-05");
  ASSERT(found);
  ASSERT_STR_EQ("obligation", testMget(found, "kind")->s);
  testNfree(arr);
  testRespFree(&r);
  PASS();
}

TEST noiseIsKeptRawButNotAnEvent(void) {
  i64 raws_before = testDbI64("SELECT COUNT(*) FROM raw_inputs;");
  Buf b = {0};
  const char *k[] = {"title", "text", "ext_id"};
  const char *v[] = {"meme of the day", "haha look at this", "t-none-1"};
  testPkInput(&b, k, v, 3);
  Resp r;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (i64)b.len, &r));
  ASSERT_EQ(200, r.status);
  Cur c = {r.body, r.body + r.len};
  Node *m = testParse(&c);
  Node *ev = testMget(m, "is_event");
  ASSERT(ev && ev->t == N_UINT && ev->u == 0);
  testNfree(m);
  testRespFree(&r);
  free(b.p);
  ASSERT_EQ(raws_before + 1, testDbI64("SELECT COUNT(*) FROM raw_inputs;"));
  ASSERT_EQ(0, testEventsTitled("meme of the day"));
  PASS();
}

TEST duplicateExtIdStoresOnce(void) {
  Buf b = {0};
  const char *k[] = {"source", "title", "text", "ext_id"};
  const char *v[] = {"notif", "Flight 2026-11-02T07:30", "boarding pass", "t-dup-1"};
  testPkInput(&b, k, v, 4);
  Resp r1, r2;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (i64)b.len, &r1));
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (i64)b.len, &r2));
  ASSERT_EQ(200, r1.status);
  ASSERT_EQ(200, r2.status);
  Cur c1 = {r1.body, r1.body + r1.len}, c2 = {r2.body, r2.body + r2.len};
  Node *m1 = testParse(&c1), *m2 = testParse(&c2);
  ASSERT(m1 && m2);
  ASSERT_EQ(testMget(m1, "raw_id")->u, testMget(m2, "raw_id")->u);
  testNfree(m1);
  testNfree(m2);
  testRespFree(&r1);
  testRespFree(&r2);
  free(b.p);
  ASSERT_EQ(1, testDbI64("SELECT COUNT(*) FROM raw_inputs WHERE ext_id='t-dup-1';"));
  ASSERT_EQ(1, testEventsTitled("Flight 2026-11-02T07:30"));
  PASS();
}

// Jev down: the raw is kept but /ingest fails, so the client retries; the
// retry answers with the raw_id stored by the failed attempt.
TEST jevFailureIs500ThenRetry(void) {
  Buf b = {0};
  const char *k[] = {"source", "title", "text", "ext_id"};
  const char *v[] = {"notif", "Meeting retry 2026-12-03T09:00", "room 4", "t-retry-1"};
  testPkInput(&b, k, v, 4);
  zenFakeMode(ZEN_FAKE_FAIL_500);
  Resp r;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (i64)b.len, &r));
  ASSERT_EQ(500, r.status);
  testRespFree(&r);
  i64 kept = testDbI64("SELECT id FROM raw_inputs WHERE ext_id='t-retry-1';");
  ASSERT(kept > 0);
  ASSERT_EQ(0, testEventsTitled("Meeting retry 2026-12-03T09:00"));
  zenFakeMode(ZEN_FAKE_OK);
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (i64)b.len, &r));
  free(b.p);
  ASSERT_EQ(200, r.status);
  Cur c = {r.body, r.body + r.len};
  Node *m = testParse(&c);
  Node *raw = testMget(m, "raw_id");
  ASSERT(raw && raw->t == N_UINT);
  ASSERT_EQ((u64)kept, raw->u);
  testNfree(m);
  testRespFree(&r);
  ASSERT_EQ(1, testEventsTitled("Meeting retry 2026-12-03T09:00"));
  PASS();
}

TEST malformedRequestIs400(void) {
  const char *bad[] = {
      "GARBAGE\r\n\r\n",
      "POST /ingest HTTP/1.1\r\nContent-Length: -5\r\n\r\n",
      "POST /ingest HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
      "POST /ingest HTTP/1.1\r\nContent-Length:\r\n\r\n",
  };
  for (usize i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    Resp r;
    ASSERT_EQ(0, testHraw(bad[i], (i64)strlen(bad[i]), NULL, 0, &r));
    ASSERT_EQ_FMT(400, r.status, "%d");
    testRespFree(&r);
  }
  PASS();
}

// ---- runner ---------------------------------------------------------------

GREATEST_MAIN_DEFS();

static void testSetup(void *arg) {
  (void)arg;
  zenFakeMode(ZEN_FAKE_OK);
}

static int testWaitHealthy(void) {
  for (i32 i = 0; i < 50; i++) {
    Resp r;
    if (!testHreq("GET", "/health", 0, NULL, 0, &r)) {
      int ok = r.status == 200;
      testRespFree(&r);
      if (ok) return 0;
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
    nanosleep(&ts, NULL);
  }
  return -1;
}

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  // free port per run: a stale server from an older run can never hijack us
  {
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t n = sizeof a;
    if (probe < 0 || bind(probe, (struct sockaddr *)&a, sizeof a) ||
        getsockname(probe, (struct sockaddr *)&a, &n)) {
      fprintf(stderr, "no free port\n");
      return 1;
    }
    test_port = ntohs(a.sin_port);
    close(probe);
  }
  char tmpl[] = "/tmp/genda-test-XXXXXX.db";
  int tfd = mkstemps(tmpl, 3);
  if (tfd < 0) {
    fprintf(stderr, "mkstemps failed\n");
    return 1;
  }
  close(tfd);
  snprintf(g_db_path, sizeof g_db_path, "%s", tmpl);
  // before fork: the child inherits GENDA_ZEN_URL and OPENCODE_API_KEY
  if (zenFakeStart()) {
    fprintf(stderr, "zen fake failed\n");
    unlink(g_db_path);
    return 1;
  }

  g_child = fork();
  if (g_child == 0) {
    char port[16];
    snprintf(port, sizeof port, "%d", test_port);
    setenv("GENDA_PORT", port, 1);
    setenv("GENDA_DB", g_db_path, 1);
    setenv("GENDA_TOKEN", TOKEN, 1);
    unsetenv("GENDA_IMAP_URL"); // dev shell may have one: never poll real mail
    execl(GENDA_BIN, "genda", (char *)NULL);
    _exit(127);
  }
  int healthy = !testWaitHealthy();
  // Port race: if our child died (e.g. bind failed), a foreign server may be
  // what answered /health. kill(pid, 0) can't tell: zombies count as alive.
  int alive = g_child > 0 && waitpid(g_child, NULL, WNOHANG) == 0;
  if (!alive) g_child = -1; // reaped or never forked: never signal a recycled pid
  int came_up = healthy && alive;
  if (came_up) {
    SET_SETUP(testSetup, NULL);
    RUN_TEST(authIsEnforced);
    RUN_TEST(notificationBecomesAppointment);
    RUN_TEST(emailBecomesObligation);
    RUN_TEST(noiseIsKeptRawButNotAnEvent);
    RUN_TEST(duplicateExtIdStoresOnce);
    RUN_TEST(jevFailureIs500ThenRetry);
    RUN_TEST(malformedRequestIs400);
  } else if (healthy) {
    fprintf(stderr, "genda exited, yet port %d answered /health: port taken?\n", test_port);
  } else {
    fprintf(stderr, "server did not come up\n");
  }
  if (g_child > 0) {
    kill(g_child, SIGTERM);
    waitpid(g_child, NULL, 0);
  }
  unlink(g_db_path);
  if (!came_up) return 1;
  GREATEST_MAIN_END();
}
