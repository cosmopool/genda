// Integration tests: real ./genda binary + temp sqlite, exercised over HTTP.
// Asserts on wire behavior (msgpack) and resulting db state.
#include <arpa/inet.h>
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

#include "../vendor/greatest.h"

#define TOKEN "t-secret"

static char g_db[256] = "";
static pid_t g_child = -1;
static int test_port = 0;

// ---- tiny http client (test side) ---------------------------------------

typedef struct {
  int status;
  unsigned char *body;
  long len;
} Resp;

static void testRespFree(Resp *r) {
  free(r->body);
  r->body = NULL;
}

// Send a raw request head (+ optional body) and read the whole response.
static int testHraw(const char *hdr, int hlen, const unsigned char *body, long blen, Resp *out) {
  memset(out, 0, sizeof *out);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval tv = {.tv_sec = 5};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)test_port)};
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof a)) {
    close(fd);
    return -1;
  }
  // (headers + optional body, always fully written)
  size_t off = 0;
  while (off < (size_t)hlen) {
    ssize_t n = send(fd, hdr + off, (size_t)hlen - off, 0);
    if (n <= 0) {
      close(fd);
      return -1;
    }
    off += (size_t)n;
  }
  off = 0;
  while (body && off < (size_t)blen) {
    ssize_t n = send(fd, body + off, (size_t)blen - off, 0);
    if (n <= 0) {
      close(fd);
      return -1;
    }
    off += (size_t)n;
  }
  size_t cap = 65536, got = 0;
  unsigned char *buf = malloc(cap);
  if (!buf) {
    close(fd);
    return -1;
  }
  ssize_t n;
  while ((n = recv(fd, buf + got, cap - got, 0)) > 0) {
    got += (size_t)n;
    if (got == cap) {
      cap *= 2;
      unsigned char *nb = realloc(buf, cap);
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
  long h = (long)(eoh - (char *)buf) + 4;
  out->len = (long)got - h;
  out->body = malloc((size_t)out->len + 1);
  if (!out->body) {
    free(buf);
    return -1;
  }
  memcpy(out->body, buf + h, (size_t)out->len);
  out->body[out->len] = 0;
  free(buf);
  return 0;
}

static int testHreq(const char *method, const char *path, int auth, const unsigned char *body,
                long blen, Resp *out) {
  char hdr[1024];
  int hlen = snprintf(hdr, sizeof hdr, "%s %s HTTP/1.1\r\nHost: x\r\n%sConnection: close\r\n\r\n",
                      method, path, auth ? "Authorization: Bearer " TOKEN "\r\n" : "");
  if (body)
    hlen = snprintf(hdr, sizeof hdr,
                    "%s %s HTTP/1.1\r\nHost: x\r\n%sContent-Type: application/msgpack\r\n"
                    "Content-Length: %ld\r\nConnection: close\r\n\r\n",
                    method, path, auth ? "Authorization: Bearer " TOKEN "\r\n" : "", blen);
  return testHraw(hdr, hlen, body, blen, out);
}

// ---- tiny msgpack (test side, independent of server impl) ----------------

typedef struct {
  unsigned char *p;
  size_t len, cap;
} Buf;

static void testBput(Buf *b, const void *s, size_t n) {
  if (b->len + n > b->cap) {
    b->cap = b->cap ? b->cap * 2 : 256;
    while (b->cap < b->len + n) b->cap *= 2;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
}

static void testPkMap(Buf *b, unsigned n) {
  unsigned char h = (unsigned char)(0x80 | n);
  testBput(b, &h, 1);
}

static void testPkStr(Buf *b, const char *s) {
  size_t n = strlen(s);
  if (n < 32) {
    unsigned char h = (unsigned char)(0xa0 | n);
    testBput(b, &h, 1);
  } else {
    unsigned char h[2] = {0xd9, (unsigned char)n};
    testBput(b, h, 2);
  }
  testBput(b, s, n);
}

// pack {k0:v0, ...} from parallel arrays
static void testPkInput(Buf *b, const char *keys[], const char *vals[], int n) {
  testPkMap(b, (unsigned)n);
  for (int i = 0; i < n; i++) {
    testPkStr(b, keys[i]);
    testPkStr(b, vals[i]);
  }
}

typedef enum { N_NIL, N_BOOL, N_UINT, N_STR, N_ARR, N_MAP, N_DBL } NodeType;
typedef struct Node {
  NodeType t;
  unsigned long long u;
  double d;
  char *s;
  struct Node **items;
  unsigned long n; // arr len, or map pair count (*2 items)
} Node;

typedef struct {
  const unsigned char *p, *end;
} Cur;

static void testNfree(Node *nd) {
  if (!nd) return;
  free(nd->s);
  for (unsigned long i = 0; i < nd->n; i++) testNfree(nd->items[i]);
  free(nd->items);
  free(nd);
}

static Node *parse(Cur *c);

static unsigned long long testBeU(Cur *c, int n) {
  unsigned long long v = 0;
  for (int i = 0; i < n; i++) v = (v << 8) | c->p[i];
  c->p += n;
  return v;
}

static Node *parse(Cur *c) {
  if (c->p >= c->end) return NULL;
  unsigned char b = *c->p++;
  Node *nd = calloc(1, sizeof *nd);
  if (!nd) return NULL;
  if ((b & 0xf0) == 0x80 || b == 0xde || b == 0xdf) {
    unsigned long n = (b & 0xf0) == 0x80 ? (unsigned)(b & 0x0f)
                                         : b == 0xde ? (unsigned)testBeU(c, 2) : (unsigned)testBeU(c, 4);
    nd->t = N_MAP;
    nd->n = n * 2;
    nd->items = calloc(nd->n ? nd->n : 1, sizeof *nd->items);
    for (unsigned long i = 0; i < nd->n; i++)
      if (!(nd->items[i] = parse(c))) {
        testNfree(nd);
        return NULL;
      }
    return nd;
  }
  if ((b & 0xf0) == 0x90 || b == 0xdc || b == 0xdd) {
    unsigned long n = (b & 0xf0) == 0x90 ? (unsigned)(b & 0x0f)
                                         : b == 0xdc ? (unsigned)testBeU(c, 2) : (unsigned)testBeU(c, 4);
    nd->t = N_ARR;
    nd->n = n;
    nd->items = calloc(n ? n : 1, sizeof *nd->items);
    for (unsigned long i = 0; i < n; i++)
      if (!(nd->items[i] = parse(c))) {
        testNfree(nd);
        return NULL;
      }
    return nd;
  }
  if ((b & 0xe0) == 0xa0 || b == 0xd9 || b == 0xda) {
    unsigned long n = (b & 0xe0) == 0xa0 ? (unsigned)(b & 0x1f)
                                         : b == 0xd9 ? (unsigned)testBeU(c, 1) : (unsigned)testBeU(c, 2);
    if (c->end - c->p < (long)n) {
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
    unsigned long long u = testBeU(c, 8);
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
  for (unsigned long i = 0; i < map->n; i += 2)
    if (map->items[i]->t == N_STR && !strcmp(map->items[i]->s, key)) return map->items[i + 1];
  return NULL;
}

static Node *testFindByTitle(Node *arr, const char *title) {
  if (!arr || arr->t != N_ARR) return NULL;
  for (unsigned long i = 0; i < arr->n; i++) {
    Node *t = testMget(arr->items[i], "title");
    if (t && t->t == N_STR && !strcmp(t->s, title)) return arr->items[i];
  }
  return NULL;
}

// ---- db peek (resulting state) -------------------------------------------

static long testDbCount(const char *sql) {
  sqlite3 *db = NULL;
  if (sqlite3_open(g_db, &db)) return -1;
  sqlite3_stmt *st = NULL;
  long n = -1;
  if (!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW)
    n = (long)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  sqlite3_close(db);
  return n;
}

// ---- cases ----------------------------------------------------------------

TEST authIsEnforced(void) {
  Resp r;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 0, (unsigned char *)"x", 1, &r));
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
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (long)b.len, &r));
  ASSERT_EQ(200, r.status);
  Cur c = {r.body, r.body + r.len};
  Node *m = parse(&c);
  ASSERT(m);
  Node *ev = testMget(m, "is_event");
  ASSERT(ev && ev->t == N_UINT && ev->u == 1);
  testNfree(m);
  testRespFree(&r);
  free(b.p);

  ASSERT_EQ(0, testHreq("GET", "/events", 1, NULL, 0, &r));
  ASSERT_EQ(200, r.status);
  Cur c2 = {r.body, r.body + r.len};
  Node *arr = parse(&c2);
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
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (long)b.len, &r));
  ASSERT_EQ(200, r.status);
  testRespFree(&r);
  free(b.p);

  ASSERT_EQ(0, testHreq("GET", "/events", 1, NULL, 0, &r));
  Cur c = {r.body, r.body + r.len};
  Node *arr = parse(&c);
  Node *found = testFindByTitle(arr, "Invoice due 2026-10-05");
  ASSERT(found);
  ASSERT_STR_EQ("obligation", testMget(found, "kind")->s);
  testNfree(arr);
  testRespFree(&r);
  PASS();
}

TEST noiseIsKeptRawButNotAnEvent(void) {
  long raws_before = testDbCount("SELECT COUNT(*) FROM raw_inputs;");
  long evs_before = testDbCount("SELECT COUNT(*) FROM events;");
  Buf b = {0};
  const char *k[] = {"title", "text", "ext_id"};
  const char *v[] = {"meme of the day", "haha look at this", "t-none-1"};
  testPkInput(&b, k, v, 3);
  Resp r;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (long)b.len, &r));
  ASSERT_EQ(200, r.status);
  Cur c = {r.body, r.body + r.len};
  Node *m = parse(&c);
  Node *ev = testMget(m, "is_event");
  ASSERT(ev && ev->t == N_UINT && ev->u == 0);
  testNfree(m);
  testRespFree(&r);
  free(b.p);
  ASSERT_EQ(raws_before + 1, testDbCount("SELECT COUNT(*) FROM raw_inputs;"));
  ASSERT_EQ(evs_before, testDbCount("SELECT COUNT(*) FROM events;"));
  PASS();
}

TEST duplicateExtIdStoresOnce(void) {
  Buf b = {0};
  const char *k[] = {"source", "title", "text", "ext_id"};
  const char *v[] = {"notif", "Flight 2026-11-02T07:30", "boarding pass", "t-dup-1"};
  testPkInput(&b, k, v, 4);
  Resp r1, r2;
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (long)b.len, &r1));
  ASSERT_EQ(0, testHreq("POST", "/ingest", 1, b.p, (long)b.len, &r2));
  ASSERT_EQ(200, r1.status);
  ASSERT_EQ(200, r2.status);
  Cur c1 = {r1.body, r1.body + r1.len}, c2 = {r2.body, r2.body + r2.len};
  Node *m1 = parse(&c1), *m2 = parse(&c2);
  ASSERT(m1 && m2);
  ASSERT_EQ(testMget(m1, "raw_id")->u, testMget(m2, "raw_id")->u);
  testNfree(m1);
  testNfree(m2);
  testRespFree(&r1);
  testRespFree(&r2);
  free(b.p);
  ASSERT_EQ(1, testDbCount("SELECT COUNT(*) FROM raw_inputs WHERE ext_id='t-dup-1';"));
  ASSERT_EQ(1, testDbCount("SELECT COUNT(*) FROM events WHERE raw_id="
                        "(SELECT id FROM raw_inputs WHERE ext_id='t-dup-1');"));
  PASS();
}

TEST malformedRequestIs400(void) {
  const char *bad[] = {
      "GARBAGE\r\n\r\n",
      "POST /ingest HTTP/1.1\r\nContent-Length: -5\r\n\r\n",
      "POST /ingest HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
      "POST /ingest HTTP/1.1\r\nContent-Length:\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    Resp r;
    ASSERT_EQ(0, testHraw(bad[i], (int)strlen(bad[i]), NULL, 0, &r));
    ASSERT_EQ_FMT(400, r.status, "%d");
    testRespFree(&r);
  }
  PASS();
}

// ---- runner ---------------------------------------------------------------

GREATEST_MAIN_DEFS();

static int testWaitHealthy(void) {
  for (int i = 0; i < 50; i++) {
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
  snprintf(g_db, sizeof g_db, "%s", tmpl);

  g_child = fork();
  if (g_child == 0) {
    char port[16];
    snprintf(port, sizeof port, "%d", test_port);
    setenv("GENDA_PORT", port, 1);
    setenv("GENDA_DB", g_db, 1);
    setenv("GENDA_TOKEN", TOKEN, 1);
    unsetenv("GENDA_IMAP_URL"); // dev shell may have one: never poll real mail
    execl("./genda", "genda", (char *)NULL);
    _exit(127);
  }
  int came_up = !testWaitHealthy() && kill(g_child, 0) == 0;
  if (came_up) {
    RUN_TEST(authIsEnforced);
    RUN_TEST(notificationBecomesAppointment);
    RUN_TEST(emailBecomesObligation);
    RUN_TEST(noiseIsKeptRawButNotAnEvent);
    RUN_TEST(duplicateExtIdStoresOnce);
    RUN_TEST(malformedRequestIs400);
  } else {
    fprintf(stderr, "server did not come up\n");
  }
  if (g_child > 0) {
    kill(g_child, SIGTERM);
    waitpid(g_child, NULL, 0);
  }
  unlink(g_db);
  if (!came_up) return 1;
  GREATEST_MAIN_END();
}
