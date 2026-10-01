// Plain-HTTP server on loopback: /health, POST /ingest, GET /events.
// TLS terminates outside (Caddy/Tailscale).
#include "server.h"

#include "classify.h"
#include "common.h"
#include "config.h"
#include "db.h"
#include "msgpack.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

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

static void serverSendAll(int fd, const void *buf, size_t n) {
  const unsigned char *p = buf;
  while (n > 0) {
    ssize_t w = send(fd, p, n, 0);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void serverReply(int fd, int code, const char *ctype, const void *body, long n) {
  const char *msg;
  switch (code) {
  case 200:
    msg = "OK";
    break;
  case 401:
    msg = "Unauthorized";
    break;
  case 404:
    msg = "Not Found";
    break;
  case 500:
    msg = "Internal Server Error";
    break;
  default:
    msg = "Bad Request";
    break;
  }
  char hdr[512];
  int hlen = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
                      code, msg, ctype, n);
  serverSendAll(fd, hdr, (size_t)hlen);
  if (n > 0) serverSendAll(fd, body, (size_t)n);
}

static int serverAuthed(const Request *r) {
  if (!g_token[0]) return 1; // dev: no token configured, allow local
  char want[768];
  snprintf(want, sizeof want, "Bearer %s", g_token);
  return strcmp(r->auth, want) == 0;
}

// Read until "\r\n\r\n" or HDR_CAP. Returns header bytes, or -1 on error.
static long serverReadHeaders(int fd, unsigned char *hdr) {
  long got = 0;
  while (got < HDR_CAP) {
    ssize_t n = recv(fd, hdr + got, (size_t)(HDR_CAP - got), 0);
    if (n <= 0) return -1;
    got += n;
    if (got >= 4 && memmem(hdr, (size_t)got, "\r\n\r\n", 4)) return got;
  }
  return -1;
}

// Percent-decode src into dst (dst size cap).
static void serverUrlDecode(const char *src, char *dst, size_t cap) {
  size_t o = 0;
  for (; *src && o + 1 < cap; src++) {
    if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
      char hex[3] = {src[1], src[2], 0};
      dst[o++] = (char)strtol(hex, NULL, 16);
      src += 2;
    } else if (*src == '+') {
      dst[o++] = ' ';
    } else {
      dst[o++] = *src;
    }
  }
  dst[o] = 0;
}

static void serverQueryVal(const char *q, const char *key, char *out, size_t cap) {
  out[0] = 0;
  size_t klen = strlen(key);
  for (const char *p = q; *p;) {
    if (!strncmp(p, key, klen) && p[klen] == '=') {
      const char *v = p + klen + 1;
      const char *e = strchr(v, '&');
      size_t n = e ? (size_t)(e - v) : strlen(v);
      char tmp[1024];
      if (n >= sizeof tmp) n = sizeof tmp - 1;
      memcpy(tmp, v, n);
      tmp[n] = 0;
      serverUrlDecode(tmp, out, cap);
      return;
    }
    p = strchr(p, '&');
    if (!p) return;
    p++;
  }
}

static void serverHandleConn(int fd) {
  static unsigned char hdr[HDR_CAP + 1];
  long hlen = serverReadHeaders(fd, hdr);
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

  // headers (bound parsing to header region so strtok_r never touches body)
  char *eoh = strstr((char *)hdr, "\r\n\r\n");
  if (!eoh) return;
  long used = (long)(eoh - (char *)hdr) + 4;
  *eoh = 0;
  char *save = NULL;
  char *line = strtok_r((char *)hdr, "\r\n", &save);
  line = strtok_r(NULL, "\r\n", &save); // skip request line
  for (; line; line = strtok_r(NULL, "\r\n", &save)) {
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
    serverReply(fd, 400, "text/plain", "body too large", 14);
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
    serverReply(fd, 200, "text/plain", "ok", 2);
  } else if (!strcmp(r.method, "GET") && !strcmp(r.path, "/events")) {
    if (!serverAuthed(&r)) {
      serverReply(fd, 401, "text/plain", "unauthorized", 12);
    } else {
      char since[64] = "", until[64] = "";
      serverQueryVal(r.query, "since", since, sizeof since);
      serverQueryVal(r.query, "until", until, sizeof until);
      MpWriter w = {0};
      if (dbPackEvents(since, until, &w)) {
        serverReply(fd, 500, "text/plain", "db error", 8);
      } else {
        serverReply(fd, 200, "application/msgpack", w.p, (long)w.len);
      }
      free(w.p);
    }
  } else if (!strcmp(r.method, "POST") && !strcmp(r.path, "/ingest")) {
    if (!serverAuthed(&r)) {
      serverReply(fd, 401, "text/plain", "unauthorized", 12);
    } else if (!strstr(r.content_type, "application/msgpack")) {
      serverReply(fd, 400, "text/plain", "want application/msgpack", 25);
    } else {
      Input in;
      if (!r.body || dbParseInput(r.body, r.body_len, &in)) {
        serverReply(fd, 400, "text/plain", "bad msgpack map", 14);
      } else {
        long long id = dbStoreRaw(&in);
        if (id < 0) {
          serverReply(fd, 500, "text/plain", "db error", 8);
        } else {
          Classified c;
          classifyInput(&in, &c);
          int is_event = c.confidence >= 0.3 && strcmp(c.kind, "none");
          int rc = 0;
          if (is_event) {
            // idempotent: re-ingest of the same raw never duplicates the event
            sqlite3_stmt *q = NULL;
            int have = 0;
            if (!sqlite3_prepare_v2(g_db, "SELECT 1 FROM events WHERE raw_id=?;", -1, &q,
                                    NULL)) {
              sqlite3_bind_int64(q, 1, id);
              have = sqlite3_step(q) == SQLITE_ROW;
              sqlite3_finalize(q);
            }
            if (!have) rc = dbStoreEvent(id, &c);
          }
          if (rc) {
            serverReply(fd, 500, "text/plain", "db error", 8);
          } else {
            MpWriter w = {0};
            mpMap(&w, 2);
            mpStr(&w, "raw_id");
            mpU64(&w, (unsigned long long)id);
            mpStr(&w, "is_event");
            mpU64(&w, is_event ? 1 : 0);
            serverReply(fd, 200, "application/msgpack", w.p, (long)w.len);
            free(w.p);
          }
        }
      }
    }
  } else {
    serverReply(fd, 404, "text/plain", "not found", 9);
  }
  free(r.body);
}

int serverRun(void) {
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
  configLog("listening on 127.0.0.1:%d db=%s", g_port, g_db_path);
  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) continue;
    serverHandleConn(fd);
    close(fd);
  }
  return 0;
}
