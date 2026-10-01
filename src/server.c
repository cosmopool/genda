// Plain-HTTP server on loopback: /health, POST /ingest, GET /events.
// TLS terminates outside (Caddy/Tailscale).
#include "server.h"

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

typedef enum { M_OTHER, M_GET, M_POST } HttpMethod;

// Parsed request head. Filled only by serverParseRequest.
typedef struct {
  HttpMethod method;
  char path[1024], query[1024];
  long content_length; // >= 0
  char auth[512];
  char content_type[128];
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

// Read until "\r\n\r\n" or HDR_CAP. Returns header bytes, 0 on error (no
// terminator in an empty block, so the caller's check covers it).
static long serverReadHeaders(int fd, unsigned char *hdr) {
  long got = 0;
  while (got < HDR_CAP) {
    ssize_t n = recv(fd, hdr + got, (size_t)(HDR_CAP - got), 0);
    if (n <= 0) return 0;
    got += n;
    if (got >= 4 && memmem(hdr, (size_t)got, "\r\n\r\n", 4)) return got;
  }
  return 0;
}

// Percent-decode the n-byte slice src into dst (dst size cap).
static void serverUrlDecode(const char *src, size_t n, char *dst, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap; i++) {
    if (src[i] == '%' && i + 2 < n && isxdigit((unsigned char)src[i + 1]) &&
        isxdigit((unsigned char)src[i + 2])) {
      char hex[3] = {src[i + 1], src[i + 2], 0};
      dst[o++] = (char)strtol(hex, NULL, 16);
      i += 2;
    } else if (src[i] == '+') {
      dst[o++] = ' ';
    } else {
      dst[o++] = src[i];
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
      serverUrlDecode(v, n, out, cap);
      return;
    }
    p = strchr(p, '&');
    if (!p) return;
    p++;
  }
}

// Parse the NUL-terminated header block (request line + header lines) into
// *r. Returns 0 ok, -1 on a malformed request line or a Content-Length that
// is not a plain non-negative number.
static int serverParseRequest(char *hdr, Request *r) {
  *r = (Request){0};
  char *save = NULL;
  char *line = strtok_r(hdr, "\r\n", &save);
  if (!line) return -1;

  // request line: METHOD SP target [SP version]
  size_t mlen = strcspn(line, " ");
  if (mlen == 0 || line[mlen] != ' ') return -1;
  const char *target = line + mlen + 1;
  size_t tlen = strcspn(target, " ");
  if (tlen == 0) return -1;
  if (mlen == 3 && !memcmp(line, "GET", 3))
    r->method = M_GET;
  else if (mlen == 4 && !memcmp(line, "POST", 4))
    r->method = M_POST;
  else
    r->method = M_OTHER;
  const char *q = memchr(target, '?', tlen);
  size_t plen = q ? (size_t)(q - target) : tlen;
  size_t qlen = q ? tlen - plen - 1 : 0;
  if (plen >= sizeof r->path || qlen >= sizeof r->query) return -1;
  memcpy(r->path, target, plen);
  r->path[plen] = 0;
  if (q) memcpy(r->query, q + 1, qlen);
  r->query[qlen] = 0;

  for (line = strtok_r(NULL, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
    if (!strncasecmp(line, "Content-Length:", 15)) {
      const char *v = line + 15;
      while (*v == ' ' || *v == '\t') v++;
      if (!isdigit((unsigned char)*v)) return -1; // empty, signed or text
      char *end = NULL;
      r->content_length = strtol(v, &end, 10); // overflow saturates past BODY_CAP
      while (*end == ' ' || *end == '\t') end++;
      if (*end) return -1;
    }
    if (!strncasecmp(line, "Authorization:", 14)) {
      const char *v = line + 14;
      while (*v == ' ') v++;
      snprintf(r->auth, sizeof r->auth, "%s", v);
    }
    if (!strncasecmp(line, "Content-Type:", 13)) {
      const char *v = line + 13;
      while (*v == ' ') v++;
      snprintf(r->content_type, sizeof r->content_type, "%s", v);
    }
  }
  return 0;
}

static void serverHandleConn(int fd) {
  static unsigned char hdr[HDR_CAP + 1]; // single-threaded server
  static unsigned char body[BODY_CAP];
  long hlen = serverReadHeaders(fd, hdr);
  hdr[hlen] = 0;

  // bound parsing to header region so strtok_r never touches body
  char *eoh = strstr((char *)hdr, "\r\n\r\n");
  if (!eoh) return;
  long used = (long)(eoh - (char *)hdr) + 4;
  *eoh = 0;
  Request r;
  if (serverParseRequest((char *)hdr, &r)) {
    serverReply(fd, 400, "text/plain", "bad request", 11);
    return;
  }

  long buffered = hlen - used;
  if (r.content_length > BODY_CAP) {
    serverReply(fd, 400, "text/plain", "body too large", 14);
    return;
  }
  // content_length is in [0, BODY_CAP] here
  long have = buffered > r.content_length ? r.content_length : buffered;
  memcpy(body, hdr + used, (size_t)have);
  while (have < r.content_length) {
    ssize_t n = recv(fd, body + have, (size_t)(r.content_length - have), 0);
    if (n <= 0) return;
    have += n;
  }

  if (r.method == M_GET && !strcmp(r.path, "/health")) {
    serverReply(fd, 200, "text/plain", "ok", 2);
  } else if (r.method == M_GET && !strcmp(r.path, "/events")) {
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
  } else if (r.method == M_POST && !strcmp(r.path, "/ingest")) {
    if (!serverAuthed(&r)) {
      serverReply(fd, 401, "text/plain", "unauthorized", 12);
    } else if (!strstr(r.content_type, "application/msgpack")) {
      serverReply(fd, 400, "text/plain", "want application/msgpack", 25);
    } else {
      Input in;
      if (dbParseInput(body, r.content_length, &in)) {
        serverReply(fd, 400, "text/plain", "bad msgpack map", 14);
      } else {
        Ingest ing = dbIngest(&in);
        if (ing.id.v == 0) {
          serverReply(fd, 500, "text/plain", "db error", 8);
        } else {
          MpWriter w = {0};
          mpMap(&w, 2);
          mpStr(&w, "raw_id");
          mpU64(&w, (unsigned long long)ing.id.v);
          mpStr(&w, "is_event");
          mpU64(&w, ing.is_event ? 1 : 0);
          serverReply(fd, 200, "application/msgpack", w.p, (long)w.len);
          free(w.p);
        }
      }
    }
  } else {
    serverReply(fd, 404, "text/plain", "not found", 9);
  }
}

int serverRun(void) {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) {
    perror("socket");
    return 1;
  }
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {0};
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
