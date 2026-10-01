// Best-effort IMAP poller feeding the same ingest path as /ingest.
// No-op unless GENDA_IMAP_URL is set. Never marks mail read.
#include "imap.h"

#include "common.h"
#include "config.h"
#include "db.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

// Response text, NUL-terminated. Allocated once in imapThread, never NULL.
typedef struct {
  char *p;
  size_t len, cap;
} CurlBuf;

static size_t imapCurlSink(void *ptr, size_t size, size_t n, void *ud) {
  CurlBuf *b = ud;
  size_t want = size * n;
  if (b->len + want + 1 > b->cap) {
    size_t ncap = b->cap * 2;
    while (ncap < b->len + want + 1) ncap *= 2;
    if (ncap > 262144) return 0;
    char *np = realloc(b->p, ncap);
    if (!np) return 0;
    b->p = np;
    b->cap = ncap;
  }
  memcpy(b->p + b->len, ptr, want);
  b->len += want;
  b->p[b->len] = 0;
  return want;
}

// One IMAP command on the shared handle (connection reused); out is reset
// and captures the untagged response text. Returns 0 ok, logs failures.
static int imapCmd(CURL *ch, const char *cmd, CurlBuf *out) {
  out->len = 0;
  out->p[0] = 0;
  curl_easy_setopt(ch, CURLOPT_CUSTOMREQUEST, cmd);
  curl_easy_setopt(ch, CURLOPT_WRITEDATA, out);
  CURLcode rc = curl_easy_perform(ch);
  if (rc != CURLE_OK) {
    configLog("imap %s: %s", cmd, curl_easy_strerror(rc));
    return -1;
  }
  return 0;
}

// Unfold + extract a header field value into out.
static void imapHdrField(const char *hdrs, const char *name, char *out, size_t cap) {
  out[0] = 0;
  size_t nlen = strlen(name);
  for (const char *p = hdrs; *p; p++) {
    if ((p == hdrs || p[-1] == '\n') && !strncasecmp(p, name, nlen) && p[nlen] == ':') {
      p += nlen + 1;
      while (*p == ' ' || *p == '\t') p++;
      size_t o = 0;
      for (; *p && *p != '\r' && *p != '\n' && o + 1 < cap; p++) out[o++] = *p;
      while (*p == '\r' || *p == '\n') { // unfolded continuation
        if ((p[1] != ' ' && p[1] != '\t') || o + 1 >= cap) break;
        out[o++] = ' ';
        p += 2;
        while (*p && *p != '\r' && *p != '\n' && o + 1 < cap) out[o++] = *p++;
      }
      out[o] = 0;
      return;
    }
  }
}

// The only code here that fills an Input. ext_id = Message-ID, else
// "imap-<uid>"; time = Date header, else now.
static Input imapParseInput(const char *hdrs, const char *text, ImapUid uid) {
  Input in = {0};
  snprintf(in.source, sizeof in.source, "email");
  imapHdrField(hdrs, "Subject", in.title, sizeof in.title);
  imapHdrField(hdrs, "From", in.from, sizeof in.from);
  imapHdrField(hdrs, "Date", in.time, sizeof in.time);
  if (!in.time[0]) dbUtcNow(in.time, sizeof in.time);
  char mid[256] = "";
  imapHdrField(hdrs, "Message-ID", mid, sizeof mid);
  if (mid[0])
    snprintf(in.ext_id, sizeof in.ext_id, "%s", mid);
  else
    snprintf(in.ext_id, sizeof in.ext_id, "imap-%ld", uid.v);
  snprintf(in.text, sizeof in.text, "%s", text);
  return in;
}

static void imapPollOnce(CURL *ch, CurlBuf *s, CurlBuf *h, CurlBuf *t) {
  if (imapCmd(ch, "UID SEARCH UNSEEN", s)) return;
  ImapUid last = dbMetaUid(), max = last;
  // response holds "* SEARCH 12 13 ..." possibly across lines, uids ascending.
  // Stop at the first failure so the next poll retries from that uid.
  char *save = NULL;
  for (char *tok = strtok_r(s->p, " \r\n", &save); tok; tok = strtok_r(NULL, " \r\n", &save)) {
    if (tok[0] < '0' || tok[0] > '9') continue;
    ImapUid uid = {atol(tok)};
    if (uid.v <= last.v) continue;
    char cmd[128], fetch[64];
    snprintf(cmd, sizeof cmd, "UID FETCH %ld BODY.PEEK[HEADER.FIELDS (MESSAGE-ID FROM SUBJECT DATE)]",
             uid.v);
    if (imapCmd(ch, cmd, h)) break;
    snprintf(fetch, sizeof fetch, "UID FETCH %ld BODY.PEEK[TEXT]", uid.v);
    if (imapCmd(ch, fetch, t)) break;
    Input in = imapParseInput(h->p, t->p, uid);
    Ingest ing = dbIngest(&in);
    if (ing.id.v == 0) {
      configLog("imap ingest failed uid=%ld", uid.v);
      break;
    }
    if (uid.v > max.v) {
      max = uid;
      dbMetaUidSet(max);
    }
    configLog("imap stored uid=%ld raw=%lld", uid.v, ing.id.v);
  }
}

void *imapThread(void *arg) {
  (void)arg;
  const char *url = configEnv("GENDA_IMAP_URL", "");
  if (!*url) return NULL; // not configured: no-op
  long every = atol(configEnv("GENDA_IMAP_POLL_SEC", "300"));
  if (every < 60) every = 60;
  // one handle and one set of buffers for the thread's lifetime
  CurlBuf s = {malloc(4096), 0, 4096}, h = {malloc(4096), 0, 4096}, t = {malloc(4096), 0, 4096};
  CURL *ch = curl_easy_init();
  if (!ch || !s.p || !h.p || !t.p) {
    configLog("imap init failed");
    curl_easy_cleanup(ch);
    free(s.p);
    free(h.p);
    free(t.p);
    return NULL;
  }
  curl_easy_setopt(ch, CURLOPT_URL, url);
  curl_easy_setopt(ch, CURLOPT_USERNAME, configEnv("GENDA_IMAP_USER", ""));
  curl_easy_setopt(ch, CURLOPT_PASSWORD, configEnv("GENDA_IMAP_PASS", ""));
  curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, imapCurlSink);
  curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(ch, CURLOPT_TIMEOUT_MS, 60000L);
  configLog("imap polling %s every %lds", url, every);
  for (;;) {
    sleep((unsigned)every);
    imapPollOnce(ch, &s, &h, &t);
  }
  return NULL;
}
