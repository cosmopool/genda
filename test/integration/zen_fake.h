// Fake OpenCode Zen System One server for tests: one thread on a free
// loopback port, answering like Jev by keywords in the request's state.
// The suites' only mock: it sits at the network boundary.
#ifndef GENDA_ZEN_FAKE_H
#define GENDA_ZEN_FAKE_H

#include "../../src/core.h"

#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "../../src/vendor/jsmn.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef enum {
  ZEN_FAKE_OK,        // Jev-like answer by keywords
  ZEN_FAKE_FAIL_500,  // HTTP 500
  ZEN_FAKE_GARBAGE,   // 200, not JSON
  ZEN_FAKE_TRUNCATED, // 200, valid prefix of an OK answer
  ZEN_FAKE_BAD_PROB,  // 200, probabilities.none = 1.7
} ZenFakeMode;

#define ZEN_FAKE_HDR_CAP 8192
#define ZEN_FAKE_BODY_CAP 65536 // > classify.c's worst-case request

static pthread_mutex_t zen_fake_mu = PTHREAD_MUTEX_INITIALIZER; // guards the two below
static ZenFakeMode zen_fake_mode = ZEN_FAKE_OK;
static char zen_fake_last[ZEN_FAKE_BODY_CAP + 1]; // last request body
static int zen_fake_srv = -1;

static inline void zenFakeMode(ZenFakeMode m) {
  pthread_mutex_lock(&zen_fake_mu);
  zen_fake_mode = m;
  pthread_mutex_unlock(&zen_fake_mu);
}

// Copy of the last request body, NUL-terminated.
static inline void zenFakeLastBody(char *out, usize cap) {
  pthread_mutex_lock(&zen_fake_mu);
  snprintf(out, cap, "%s", zen_fake_last);
  pthread_mutex_unlock(&zen_fake_mu);
}

// Does the request's state object mention any of words (lowercase)?
// Only state: the questions' own text names meetings and deadlines.
static inline int zenFakeStateHas(const char *js, const jsmntok_t *t, int n, const char *words[]) {
  static char low[ZEN_FAKE_BODY_CAP + 1]; // fake thread only
  for (int i = 1; i + 1 < n; i++) {
    if (t[i].parent != 0 || t[i].end - t[i].start != 5 || memcmp(js + t[i].start, "state", 5)) continue;
    usize len = (usize)(t[i + 1].end - t[i + 1].start);
    for (usize j = 0; j < len; j++) low[j] = (char)tolower((unsigned char)js[t[i + 1].start + (int)j]);
    low[len] = 0;
    for (int w = 0; words[w]; w++)
      if (strstr(low, words[w])) return 1;
  }
  return 0;
}

static inline void zenFakeReply(int fd, int status, const char *body, usize len) {
  char hdr[256];
  int h = snprintf(hdr, sizeof hdr,
                   "HTTP/1.1 %d Fake\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                   "Connection: close\r\n\r\n",
                   status, len);
  send(fd, hdr, (usize)h, 0);
  for (usize off = 0; off < len;) {
    ssize_t w = send(fd, body + off, len - off, 0);
    if (w <= 0) return;
    off += (usize)w;
  }
}

// One request per connection: read head + Content-Length body, record the
// body, answer by mode.
static inline void zenFakeServe(int fd) {
  static char req[ZEN_FAKE_HDR_CAP + ZEN_FAKE_BODY_CAP + 1]; // fake thread only
  static jsmntok_t tok[128];
  usize got = 0;
  char *eoh = NULL;
  while (!eoh && got < ZEN_FAKE_HDR_CAP) {
    ssize_t r = recv(fd, req + got, ZEN_FAKE_HDR_CAP - got, 0);
    if (r <= 0) return;
    got += (usize)r;
    req[got] = 0;
    eoh = strstr(req, "\r\n\r\n");
  }
  if (!eoh) return;
  usize head = (usize)(eoh - req) + 4;
  long clen = 0;
  char auth[256] = "";
  for (char *line = req; line < eoh;) {
    char *nl = strstr(line, "\r\n");
    if (!strncasecmp(line, "Content-Length:", 15)) clen = strtol(line + 15, NULL, 10);
    if (!strncasecmp(line, "Authorization:", 14)) {
      const char *v = line + 14;
      while (*v == ' ') v++;
      snprintf(auth, sizeof auth, "%.*s", (int)(nl - v), v);
    }
    line = nl + 2;
  }
  if (clen < 0 || clen > ZEN_FAKE_BODY_CAP) {
    zenFakeReply(fd, 413, "{\"error\":\"too large\"}", 21);
    return;
  }
  while (got < head + (usize)clen) {
    ssize_t r = recv(fd, req + got, head + (usize)clen - got, 0);
    if (r <= 0) return;
    got += (usize)r;
  }
  char *body = req + head;
  body[clen] = 0;
  pthread_mutex_lock(&zen_fake_mu);
  memcpy(zen_fake_last, body, (usize)clen + 1);
  ZenFakeMode mode = zen_fake_mode;
  pthread_mutex_unlock(&zen_fake_mu);

  if (strcmp(auth, "Bearer test-key")) {
    zenFakeReply(fd, 401, "{\"error\":\"unauthorized\"}", 24);
    return;
  }
  if (mode == ZEN_FAKE_FAIL_500) {
    zenFakeReply(fd, 500, "{\"error\":\"internal\"}", 20);
    return;
  }
  if (mode == ZEN_FAKE_GARBAGE) {
    zenFakeReply(fd, 200, "<html>not json</html>", 21);
    return;
  }
  jsmn_parser p;
  jsmn_init(&p);
  int n = jsmn_parse(&p, body, (usize)clen, tok, sizeof tok / sizeof tok[0]);
  if (n < 1 || tok[0].type != JSMN_OBJECT) { // like Zen: bad JSON is a 422
    zenFakeReply(fd, 422, "{\"error\":\"invalid json\"}", 24);
    return;
  }
  static const char *appt[] = {"dentist", "flight", "meeting", NULL};
  static const char *oblg[] = {"invoice", "rent", "due", NULL};
  static const char *dl[] = {"due", "deadline", NULL};
  const char *choice = "none", *pa = "0.05", *po = "0.05", *pn = "0.9";
  if (zenFakeStateHas(body, tok, n, appt)) {
    choice = "appointment";
    pa = "0.9";
    pn = "0.05";
  } else if (zenFakeStateHas(body, tok, n, oblg)) {
    choice = "obligation";
    po = "0.9";
    pn = "0.05";
  }
  if (mode == ZEN_FAKE_BAD_PROB) pn = "1.7";
  char noul[128] = "";
  if (strstr(body, "is_deadline"))
    snprintf(noul, sizeof noul, ",\"is_deadline\":{\"type\":\"noul\",\"noul\":%s}",
             zenFakeStateHas(body, tok, n, dl) ? "0.9" : "0.1");
  char out[1024];
  int len = snprintf(out, sizeof out,
                     "{\"model\":\"jev-1.13.0\",\"answers\":{\"kind\":{\"type\":\"choice\",\"choice\":\"%s\","
                     "\"probabilities\":{\"appointment\":%s,\"obligation\":%s,\"none\":%s},"
                     "\"confidence\":0.81}%s},\"usage\":{\"input_tokens\":120,\"output_tokens\":4}}",
                     choice, pa, po, pn, noul);
  if (mode == ZEN_FAKE_TRUNCATED) len /= 2;
  zenFakeReply(fd, 200, out, (usize)len);
}

static inline void *zenFakeThread(void *arg) {
  (void)arg;
  for (;;) {
    int fd = accept(zen_fake_srv, NULL, NULL);
    if (fd < 0) continue;
    struct timeval tv = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    zenFakeServe(fd);
    close(fd);
  }
  return NULL;
}

// Listen on a free loopback port, serve from a detached thread, and point
// this process (and children it spawns) at it: GENDA_ZEN_URL,
// OPENCODE_API_KEY=test-key, no proxy. Returns 0 ok.
static inline int zenFakeStart(void) {
  zen_fake_srv = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0};
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof a;
  if (zen_fake_srv < 0 || bind(zen_fake_srv, (struct sockaddr *)&a, sizeof a) ||
      getsockname(zen_fake_srv, (struct sockaddr *)&a, &alen) || listen(zen_fake_srv, 16))
    return -1;
  fcntl(zen_fake_srv, F_SETFD, FD_CLOEXEC); // a spawned genda must not hold it
  pthread_t th;
  if (pthread_create(&th, NULL, zenFakeThread, NULL)) return -1;
  pthread_detach(th);
  char url[128];
  snprintf(url, sizeof url, "http://127.0.0.1:%d/zen/v1/systemone", ntohs(a.sin_port));
  setenv("GENDA_ZEN_URL", url, 1);
  setenv("OPENCODE_API_KEY", "test-key", 1);
  setenv("no_proxy", "*", 1); // a dev shell proxy must not see test traffic
  return 0;
}

#endif
