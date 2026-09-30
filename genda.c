// genda - personal agenda server: ingest notifications+email, classify into
// events, serve msgpack feed. Single file, POSIX sockets, sqlite, libcurl.
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

static int g_port = 8080;
static char g_db_path[1024] = "./genda.db";
static char g_token[256] = "";
static sqlite3 *g_db = NULL;

static void logf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[genda] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

static const char *env_or(const char *k, const char *dflt) {
  const char *v = getenv(k);
  return (v && *v) ? v : dflt;
}

// ---- msgpack (minimal subset: nil bool uint int float str bin map array) ---

typedef struct {
  unsigned char *p;
  size_t len, cap;
} MPW;

static void mp_reserve(MPW *w, size_t extra) {
  if (w->len + extra <= w->cap) return;
  size_t ncap = w->cap ? w->cap * 2 : 256;
  while (ncap < w->len + extra) ncap *= 2;
  w->p = realloc(w->p, ncap);
  w->cap = ncap;
}

static void mp_b(MPW *w, unsigned char b) {
  mp_reserve(w, 1);
  w->p[w->len++] = b;
}

static void mp_map(MPW *w, unsigned long n) {
  if (n < 16) return mp_b(w, (unsigned char)(0x80 | n));
  mp_reserve(w, 3);
  w->p[w->len++] = 0xde;
  w->p[w->len++] = (unsigned char)(n >> 8);
  w->p[w->len++] = (unsigned char)n;
}

static void mp_arr(MPW *w, unsigned long n) {
  if (n < 16) return mp_b(w, (unsigned char)(0x90 | n));
  mp_reserve(w, 3);
  w->p[w->len++] = 0xdc;
  w->p[w->len++] = (unsigned char)(n >> 8);
  w->p[w->len++] = (unsigned char)n;
}

static void mp_str(MPW *w, const char *s) {
  size_t n = strlen(s);
  if (n < 32) {
    mp_b(w, (unsigned char)(0xa0 | n));
  } else if (n < 256) {
    mp_reserve(w, 2);
    w->p[w->len++] = 0xd9;
    w->p[w->len++] = (unsigned char)n;
  } else {
    mp_reserve(w, 3);
    w->p[w->len++] = 0xda;
    w->p[w->len++] = (unsigned char)(n >> 8);
    w->p[w->len++] = (unsigned char)n;
  }
  mp_reserve(w, n);
  memcpy(w->p + w->len, s, n);
  w->len += n;
}

static void mp_u64(MPW *w, unsigned long long v) {
  if (v < 128) return mp_b(w, (unsigned char)v);
  mp_reserve(w, 9);
  w->p[w->len++] = 0xcf;
  for (int i = 7; i >= 0; i--) w->p[w->len++] = (unsigned char)(v >> (i * 8));
}

static void mp_f64(MPW *w, double v) {
  mp_reserve(w, 9);
  w->p[w->len++] = 0xcb;
  unsigned long long u;
  memcpy(&u, &v, 8);
  for (int i = 7; i >= 0; i--) w->p[w->len++] = (unsigned char)(u >> (i * 8));
}

typedef struct {
  const unsigned char *p, *end;
} MPR;

static int mp_byte(MPR *r, unsigned char *out) {
  if (r->p >= r->end) return -1;
  *out = *r->p++;
  return 0;
}

// If next value is map/array header, return length. Else -1.
static long mp_hdr_len(MPR *r, unsigned char fix, unsigned char b16, unsigned char b32) {
  unsigned char b;
  if (mp_byte(r, &b)) return -1;
  if ((b & 0xf0) == fix) return b & 0x0f;
  if (b == b16) {
    if (r->end - r->p < 2) return -1;
    long n = (r->p[0] << 8) | r->p[1];
    r->p += 2;
    return n;
  }
  if (b == b32) {
    if (r->end - r->p < 4) return -1;
    long n = ((long)r->p[0] << 24) | (r->p[1] << 16) | (r->p[2] << 8) | r->p[3];
    r->p += 4;
    return n;
  }
  return -1;
}

static int mp_skip(MPR *r);

static int mp_skip_n(MPR *r, long n) {
  if (r->end - r->p < n) return -1;
  r->p += n;
  return 0;
}

static int mp_skip(MPR *r) {
  if (r->p >= r->end) return -1;
  unsigned char b = *r->p;
  if (b < 0x80 || (b >= 0xe0)) {
    r->p++;
    return 0;
  }
  if ((b & 0xe0) == 0xa0) { // fixstr
    long n = b & 0x1f;
    r->p++;
    return mp_skip_n(r, n);
  }
  if ((b & 0xf0) == 0x80) { // fixmap
    long n = b & 0x0f;
    r->p++;
    for (long i = 0; i < n; i++)
      if (mp_skip(r) || mp_skip(r)) return -1;
    return 0;
  }
  if ((b & 0xf0) == 0x90) { // fixarray
    long n = b & 0x0f;
    r->p++;
    for (long i = 0; i < n; i++)
      if (mp_skip(r)) return -1;
    return 0;
  }
  r->p++;
  switch (b) {
  case 0xc0: // nil
  case 0xc2: // false
  case 0xc3: // true
    return 0;
  case 0xcc:
    return mp_skip_n(r, 1);
  case 0xcd:
    return mp_skip_n(r, 2);
  case 0xce:
    return mp_skip_n(r, 4);
  case 0xcf:
  case 0xd3:
  case 0xcb:
    return mp_skip_n(r, 8);
  case 0xd0:
    return mp_skip_n(r, 1);
  case 0xd1:
    return mp_skip_n(r, 2);
  case 0xd2:
    return mp_skip_n(r, 4);
  case 0xca:
    return mp_skip_n(r, 4);
  case 0xd9: {
    unsigned char n;
    if (mp_byte(r, &n)) return -1;
    return mp_skip_n(r, n);
  }
  case 0xda:
  case 0xdb: {
    // str16/32
    long n = -1;
    if (b == 0xda) {
      if (r->end - r->p < 2) return -1;
      n = (r->p[0] << 8) | r->p[1];
      r->p += 2;
    } else {
      if (r->end - r->p < 4) return -1;
      n = ((long)r->p[0] << 24) | (r->p[1] << 16) | (r->p[2] << 8) | r->p[3];
      r->p += 4;
    }
    return mp_skip_n(r, n);
  }
  case 0xdc:
  case 0xdd: {
    MPR t = *r;
    t.p--;
    long n = mp_hdr_len(&t, 0, 0xdc, 0xdd);
    if (n < 0) return -1;
    *r = t;
    for (long i = 0; i < n; i++)
      if (mp_skip(r)) return -1;
    return 0;
  }
  case 0xde:
  case 0xdf: {
    MPR t = *r;
    t.p--;
    long n = mp_hdr_len(&t, 0, 0xde, 0xdf);
    if (n < 0) return -1;
    *r = t;
    for (long i = 0; i < n; i++)
      if (mp_skip(r) || mp_skip(r)) return -1;
    return 0;
  }
  case 0xc4:
  case 0xc5:
  case 0xc6: {
    // bin8/16/32
    long n = -1;
    if (b == 0xc4) {
      unsigned char m;
      if (mp_byte(r, &m)) return -1;
      n = m;
    } else if (b == 0xc5) {
      if (r->end - r->p < 2) return -1;
      n = (r->p[0] << 8) | r->p[1];
      r->p += 2;
    } else {
      if (r->end - r->p < 4) return -1;
      n = ((long)r->p[0] << 24) | (r->p[1] << 16) | (r->p[2] << 8) | r->p[3];
      r->p += 4;
    }
    return mp_skip_n(r, n);
  }
  default:
    return -1;
  }
}

// Read next value as NUL-terminated string (str/bin families) or scalar
// rendered as text (uint/int/float/bool/nil->""). Returns malloc'd buf.
static char *mp_strval(MPR *r) {
  if (r->p >= r->end) return NULL;
  unsigned char b = *r->p;
  long n;
  if ((b & 0xe0) == 0xa0 || b == 0xd9) {
    MPR t = *r;
    t.p++;
    if ((b & 0xe0) == 0xa0)
      n = b & 0x1f;
    else if (mp_byte(&t, (unsigned char *)&n))
      return NULL;
    if (t.end - t.p < n) return NULL;
    char *s = malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, t.p, (size_t)n);
    s[n] = 0;
    r->p = t.p + n;
    return s;
  }
  if (b == 0xda || b == 0xdb || b == 0xc4 || b == 0xc5 || b == 0xc6) {
    r->p++;
    if (b == 0xda || b == 0xc5) {
      if (r->end - r->p < 2) return NULL;
      n = (r->p[0] << 8) | r->p[1];
      r->p += 2;
    } else if (b == 0xdb || b == 0xc6) {
      if (r->end - r->p < 4) return NULL;
      n = ((long)r->p[0] << 24) | (r->p[1] << 16) | (r->p[2] << 8) | r->p[3];
      r->p += 4;
    } else {
      unsigned char m;
      if (mp_byte(r, &m)) return NULL;
      n = m;
    }
    if (r->end - r->p < n) return NULL;
    char *s = malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, r->p, (size_t)n);
    s[n] = 0;
    r->p += n;
    return s;
  }
  if (b == 0xc0) {
    r->p++;
    char *s = malloc(1);
    if (s) s[0] = 0;
    return s;
  }
  if (b == 0xc2 || b == 0xc3) {
    r->p++;
    return strdup(b == 0xc3 ? "true" : "false");
  }
  if (b < 0x80 || b >= 0xe0 || b == 0xcc || b == 0xcd || b == 0xce || b == 0xcf || b == 0xd0 ||
      b == 0xd1 || b == 0xd2 || b == 0xd3) {
    unsigned long long u = 0;
    long long s = 0;
    int neg = 0;
    r->p++;
    if (b < 0x80)
      u = b;
    else if (b >= 0xe0)
      s = (signed char)b, neg = 1;
    else if (b == 0xcc) {
      unsigned char m;
      if (mp_byte(r, &m)) return NULL;
      u = m;
    } else if (b == 0xcd) {
      if (r->end - r->p < 2) return NULL;
      u = ((unsigned)r->p[0] << 8) | r->p[1];
      r->p += 2;
    } else if (b == 0xce) {
      if (r->end - r->p < 4) return NULL;
      u = ((unsigned long long)r->p[0] << 24) | ((unsigned long long)r->p[1] << 16) |
          ((unsigned long long)r->p[2] << 8) | r->p[3];
      r->p += 4;
    } else if (b == 0xcf) {
      if (r->end - r->p < 8) return NULL;
      for (int i = 0; i < 8; i++) u = (u << 8) | *r->p++;
    } else if (b == 0xd0) {
      signed char m;
      if (mp_byte(r, (unsigned char *)&m)) return NULL;
      s = m, neg = 1;
    } else if (b == 0xd1) {
      if (r->end - r->p < 2) return NULL;
      s = (short)((r->p[0] << 8) | r->p[1]), neg = 1;
      r->p += 2;
    } else if (b == 0xd2) {
      if (r->end - r->p < 4) return NULL;
      s = ((long)r->p[0] << 24) | (r->p[1] << 16) | (r->p[2] << 8) | r->p[3], neg = 1;
      r->p += 4;
    } else {
      if (r->end - r->p < 8) return NULL;
      unsigned long long m = 0;
      for (int i = 0; i < 8; i++) m = (m << 8) | *r->p++;
      s = (long long)m, neg = 1;
    }
    char tmp[32];
    if (neg)
      snprintf(tmp, sizeof tmp, "%lld", s);
    else
      snprintf(tmp, sizeof tmp, "%llu", u);
    return strdup(tmp);
  }
  if (b == 0xca || b == 0xcb) {
    r->p++;
    double v = 0;
    if (b == 0xca) {
      if (r->end - r->p < 4) return NULL;
      unsigned u = ((unsigned)r->p[0] << 24) | ((unsigned)r->p[1] << 16) | ((unsigned)r->p[2] << 8) |
                   r->p[3];
      r->p += 4;
      float f;
      memcpy(&f, &u, 4);
      v = f;
    } else {
      if (r->end - r->p < 8) return NULL;
      unsigned long long u = 0;
      for (int i = 0; i < 8; i++) u = (u << 8) | *r->p++;
      memcpy(&v, &u, 8);
    }
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%g", v);
    return strdup(tmp);
  }
  return NULL;
}

// ---- db -----------------------------------------------------------------

static void utc_now(char *out, size_t n) {
  time_t t = time(NULL);
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int db_open(void) {
  if (sqlite3_open(g_db_path, &g_db)) {
    logf("sqlite open %s: %s", g_db_path, sqlite3_errmsg(g_db));
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
    logf("schema: %s", err ? err : "?");
    sqlite3_free(err);
    return -1;
  }
  return 0;
}

typedef struct {
  char source[32], app[128], title[512], text[4096], from[256], time[64], ext_id[128];
} Input;

static void djb_hex(const char *a, const char *b, const char *c, char *out, size_t n) {
  unsigned long long h = 5381;
  for (const char *s = a; s && *s; s++) h = h * 33 + (unsigned char)*s;
  for (const char *s = b; s && *s; s++) h = h * 33 + (unsigned char)*s;
  for (const char *s = c; s && *s; s++) h = h * 33 + (unsigned char)*s;
  snprintf(out, n, "%016llx", h);
}

// Parse msgpack map body into Input. Unknown keys skipped. Returns 0 ok.
static int parse_input(const unsigned char *body, long len, Input *in) {
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
static long long store_raw(const Input *in) {
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

// ---- http ---------------------------------------------------------------

#define HDR_CAP 8192
#define BODY_CAP (1 << 20)

typedef struct {
  char method[8], path[1024], query[1024];
  long content_length;
  char auth[512];
  char content_type[128];
  unsigned char *body;
  long body_len;
} Request;

static void send_all(int fd, const void *buf, size_t n) {
  const unsigned char *p = buf;
  while (n > 0) {
    ssize_t w = send(fd, p, n, 0);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void reply(int fd, int code, const char *ctype, const void *body, long n) {
  const char *msg = code == 200 ? "OK" : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : "Bad Request";
  char hdr[512];
  int hlen = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                      code, msg, ctype, n);
  send_all(fd, hdr, (size_t)hlen);
  if (n > 0) send_all(fd, body, (size_t)n);
}

static int authed(const Request *r) {
  if (!g_token[0]) return 1; // dev: no token configured, allow local
  char want[768];
  snprintf(want, sizeof want, "Bearer %s", g_token);
  return strcmp(r->auth, want) == 0;
}

// Read until "\r\n\r\n" or HDR_CAP. Returns header bytes, or -1 on error.
static long read_headers(int fd, unsigned char *hdr) {
  long got = 0;
  while (got < HDR_CAP) {
    ssize_t n = recv(fd, hdr + got, (size_t)(HDR_CAP - got), 0);
    if (n <= 0) return -1;
    got += n;
    if (got >= 4 && memmem(hdr, (size_t)got, "\r\n\r\n", 4)) return got;
  }
  return -1;
}

static void handle_conn(int fd) {
  static unsigned char hdr[HDR_CAP + 1];
  long hlen = read_headers(fd, hdr);
  if (hlen < 0) return;
  hdr[hlen] = 0;

  Request r;
  memset(&r, 0, sizeof r);
  char target[2048] = "";
  sscanf((char *)hdr, "%7s %2047s", r.method, target);
  char *q = strchr(target, '?');
  if (q) {
    *q = 0;
    snprintf(r.query, sizeof r.query, "%s", q + 1);
  }
  snprintf(r.path, sizeof r.path, "%s", target);

  // headers (bound parsing to header region so strtok never touches body)
  char *eoh = strstr((char *)hdr, "\r\n\r\n");
  if (!eoh) return;
  long used = (long)(eoh - (char *)hdr) + 4;
  *eoh = 0;
  char *line = strtok((char *)hdr, "\r\n");
  line = strtok(NULL, "\r\n"); // skip request line
  for (; line; line = strtok(NULL, "\r\n")) {
    if (!*line) break;
    if (!strncasecmp(line, "Content-Length:", 15)) r.content_length = atol(line + 15);
    if (!strncasecmp(line, "Authorization:", 14)) {
      const char *v = line + 14;
      while (*v == ' ') v++;
      snprintf(r.auth, sizeof r.auth, "%s", v);
    }
    if (!strncasecmp(line, "Content-Type:", 13)) {
      const char *v = line + 13;
      while (*v == ' ') v++;
      snprintf(r.content_type, sizeof r.content_type, "%s", v);
    }
  }

  long buffered = hlen - used;
  if (r.content_length > BODY_CAP) {
    reply(fd, 400, "text/plain", "body too large", 14);
    return;
  }
  if (r.content_length > 0) {
    r.body = malloc((size_t)r.content_length);
    if (!r.body) return;
    long have = buffered > r.content_length ? r.content_length : buffered;
    memcpy(r.body, hdr + used, (size_t)have);
    while (have < r.content_length) {
      ssize_t n = recv(fd, r.body + have, (size_t)(r.content_length - have), 0);
      if (n <= 0) {
        free(r.body);
        return;
      }
      have += n;
    }
    r.body_len = r.content_length;
  }

  if (!strcmp(r.method, "GET") && !strcmp(r.path, "/health")) {
    reply(fd, 200, "text/plain", "ok", 2);
  } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/ingest")) {
    if (!authed(&r)) {
      reply(fd, 401, "text/plain", "unauthorized", 12);
    } else if (!strstr(r.content_type, "application/msgpack")) {
      reply(fd, 400, "text/plain", "want application/msgpack", 25);
    } else {
      Input in;
      if (!r.body || parse_input(r.body, r.body_len, &in)) {
        reply(fd, 400, "text/plain", "bad msgpack map", 14);
      } else {
        long long id = store_raw(&in);
        if (id < 0) {
          reply(fd, 500, "text/plain", "db error", 8);
        } else {
          MPW w = {0};
          mp_map(&w, 1);
          mp_str(&w, "raw_id");
          mp_u64(&w, (unsigned long long)id);
          reply(fd, 200, "application/msgpack", w.p, (long)w.len);
          free(w.p);
        }
      }
    }
  } else {
    reply(fd, 404, "text/plain", "not found", 9);
  }
  free(r.body);
}

int main(void) {
  g_port = atoi(env_or("GENDA_PORT", "8080"));
  snprintf(g_db_path, sizeof g_db_path, "%s", env_or("GENDA_DB", "./genda.db"));
  snprintf(g_token, sizeof g_token, "%s", getenv("GENDA_TOKEN") ? getenv("GENDA_TOKEN") : "");
  curl_global_init(CURL_GLOBAL_ALL);
  if (db_open()) return 1;

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    return 1;
  }
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)g_port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
    perror("bind");
    return 1;
  }
  if (listen(srv, 64) < 0) {
    perror("listen");
    return 1;
  }
  logf("listening on 127.0.0.1:%d db=%s", g_port, g_db_path);
  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) continue;
    handle_conn(fd);
    close(fd);
  }
  return 0;
}
