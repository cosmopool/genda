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

  // headers
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

  long used = (long)((strstr((char *)hdr, "\r\n\r\n") - (char *)hdr) + 4);
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
