// Best-effort IMAP poller feeding the same ingest path as /ingest.
// No-op unless GENDA_IMAP_URL is set. Never marks mail read.
#include "imap.h"

#include "classify.h"
#include "common.h"
#include "config.h"
#include "db.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef struct {
  char *p;
  size_t len, cap;
} CurlBuf;

static size_t imapCurlSink(void *ptr, size_t size, size_t n, void *ud) {
  CurlBuf *b = ud;
  size_t want = size * n;
  if (b->len + want + 1 > b->cap) {
    size_t ncap = b->cap ? b->cap * 2 : 4096;
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

// One IMAP command; captures the untagged response text. Returns 0 ok.
static int imapCmd(const char *url, const char *user, const char *pass, const char *cmd,
                    CurlBuf *out) {
  CURL *ch = curl_easy_init();
  if (!ch) return -1;
  curl_easy_setopt(ch, CURLOPT_URL, url);
  curl_easy_setopt(ch, CURLOPT_USERNAME, user);
  curl_easy_setopt(ch, CURLOPT_PASSWORD, pass);
  curl_easy_setopt(ch, CURLOPT_CUSTOMREQUEST, cmd);
  curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, imapCurlSink);
  curl_easy_setopt(ch, CURLOPT_WRITEDATA, out);
  curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(ch, CURLOPT_TIMEOUT_MS, 60000L);
  CURLcode rc = curl_easy_perform(ch);
  curl_easy_cleanup(ch);
  return rc == CURLE_OK ? 0 : -1;
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

static void imapPollOnce(const char *url, const char *user, const char *pass) {
  CurlBuf s = {0};
  if (imapCmd(url, user, pass, "UID SEARCH UNSEEN", &s)) {
    configLog("imap search failed");
    free(s.p);
    return;
  }
  long last = dbMetaUid(), max = last;
  // response holds "* SEARCH 12 13 ..." possibly across lines
  for (char *tok = strtok(s.p, " \r\n"); tok; tok = strtok(NULL, " \r\n")) {
    if (tok[0] < '0' || tok[0] > '9') continue;
    long uid = atol(tok);
    if (uid <= last) continue;
    char cmd[64], fetch[64];
    snprintf(cmd, sizeof cmd, "UID FETCH %ld BODY.PEEK[HEADER.FIELDS (MESSAGE-ID FROM SUBJECT DATE)]",
             uid);
    CurlBuf h = {0};
    if (imapCmd(url, user, pass, cmd, &h)) {
      free(h.p);
      continue;
    }
    snprintf(fetch, sizeof fetch, "UID FETCH %ld BODY.PEEK[TEXT]", uid);
    CurlBuf t = {0};
    if (imapCmd(url, user, pass, fetch, &t)) {
      free(h.p);
      free(t.p);
      continue;
    }
    Input in;
    memset(&in, 0, sizeof in);
    snprintf(in.source, sizeof in.source, "email");
    imapHdrField(h.p ? h.p : "", "Subject", in.title, sizeof in.title);
    imapHdrField(h.p ? h.p : "", "From", in.from, sizeof in.from);
    imapHdrField(h.p ? h.p : "", "Date", in.time, sizeof in.time);
    char mid[256] = "";
    imapHdrField(h.p ? h.p : "", "Message-ID", mid, sizeof mid);
    if (mid[0])
      snprintf(in.ext_id, sizeof in.ext_id, "%s", mid);
    else
      snprintf(in.ext_id, sizeof in.ext_id, "imap-%ld", uid);
    if (t.p) snprintf(in.text, sizeof in.text, "%s", t.p);
    free(h.p);
    free(t.p);
    long long id = dbStoreRaw(&in);
    if (id > 0) {
      Classified c;
      classifyInput(&in, &c);
      if (c.confidence >= 0.3 && strcmp(c.kind, "none")) dbStoreEvent(id, &c);
    }
    if (uid > max) {
      max = uid;
      dbMetaUidSet(max);
    }
    configLog("imap stored uid=%ld raw=%lld", uid, id);
  }
  free(s.p);
}

void *imapThread(void *arg) {
  (void)arg;
  const char *url = getenv("GENDA_IMAP_URL");
  if (!url || !*url) return NULL; // not configured: no-op
  const char *user = configEnv("GENDA_IMAP_USER", "");
  const char *pass = configEnv("GENDA_IMAP_PASS", "");
  long every = atol(configEnv("GENDA_IMAP_POLL_SEC", "300"));
  if (every < 60) every = 60;
  configLog("imap polling %s every %lds", url, every);
  for (;;) {
    sleep((unsigned)every);
    imapPollOnce(url, user, pass);
  }
  return NULL;
}
