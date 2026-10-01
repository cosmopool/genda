// Input classification into calendar events via Jev (OpenCode Zen System
// One). Stores nothing; sets is_event when an events row should be written.
// One shared curl handle and fixed static buffers, all behind classify_mu:
// the server thread and the IMAP thread both classify.
#include "classify.h"

#include "common.h"
#include "config.h"

#include <ctype.h>
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "vendor/jsmn.h"

#define CLASSIFY_RESP_CAP 16384 // a Jev reply is ~300 bytes; more is not Jev
#define CLASSIFY_TOK_CAP 256    // a full reply is ~30 tokens

// JSON-escaped copies of the Input fields sent as state. 6x: the worst
// case is every byte a control byte written as \u00XX.
static struct {
  char source[6 * sizeof((Input *)0)->source], app[6 * sizeof((Input *)0)->app],
      from[6 * sizeof((Input *)0)->from], title[6 * sizeof((Input *)0)->title],
      text[6 * sizeof((Input *)0)->text];
} classify_esc;
static char classify_model[6 * 128];                  // escaped GENDA_ZEN_MODEL
static char classify_req[sizeof classify_esc + 8192]; // + fixed text, model, date
static char classify_resp[CLASSIFY_RESP_CAP + 1];     // NUL-terminated
static usize classify_resp_len;
static jsmntok_t classify_tok[CLASSIFY_TOK_CAP + 1];  // + the nil token
static CURL *classify_curl;
static struct curl_slist *classify_hdrs;
static pthread_mutex_t classify_mu = PTHREAD_MUTEX_INITIALIZER;

// Jev is literal: say exactly what each answer means.
#define CLASSIFY_REQ_FMT                                                                                       \
  "{\"model\":\"%s\","                                                                                         \
  "\"state\":{\"source\":\"%s\",\"app\":\"%s\",\"from\":\"%s\",\"title\":\"%s\",\"text\":\"%s\"},"             \
  "\"questions\":{\"kind\":{\"type\":\"choice\","                                                              \
  "\"instructions\":\"Classify this message (an email or a phone notification) by what it puts on the "        \
  "reader's calendar.\","                                                                                      \
  "\"criteria\":{"                                                                                             \
  "\"appointment\":\"A meeting, visit, call, trip or event that happens at a time and that the reader "        \
  "attends or takes part in.\","                                                                               \
  "\"obligation\":\"Something the reader must pay, submit, renew or do by a date.\","                          \
  "\"none\":\"Anything else: news, chat, ads, receipts, or notifications that need no action from the "        \
  "reader.\"}}%s}}"
#define CLASSIFY_DEADLINE_FMT                                                                                  \
  ",\"is_deadline\":{\"type\":\"noul\","                                                                       \
  "\"instructions\":{\"date\":\"%s\",\"question\":\"Is `date` a deadline by which something must be done, "    \
  "rather than the time something starts?\"},"                                                                 \
  "\"criteria\":{\"true\":\"`date` is a due date, expiry or cutoff: something must be paid, submitted, "       \
  "renewed or done by then.\","                                                                                \
  "\"false\":\"`date` is when something starts or happens, such as a meeting, visit, call or trip.\"}}"

// First YYYY-MM-DD([T ]HH:MM) occurrence -> out. Returns 1 found.
static int classifyScanIsoDate(const char *s, char *out, usize cap) {
  for (; *s; s++) {
    // sscanf skips leading whitespace, which would shift the s[10]/s+11
    // offsets below — only match where the date literally starts.
    if (!isdigit((unsigned char)*s)) continue;
    int year, month, day, h = -1, m = -1;
    int got = sscanf(s, "%4d-%2d-%2d", &year, &month, &day);
    int valid = got == 3 && year >= 2020 && year <= 2100 &&
                month >= 1 && month <= 12 && day >= 1 && day <= 31;
    if (valid) {
      if ((s[10] == 'T' || s[10] == ' ') && sscanf(s + 11, "%2d:%2d", &h, &m) == 2 && h >= 0 &&
          h < 24 && m >= 0 && m < 60)
        snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:00Z", year, month, day, h, m);
      else
        snprintf(out, cap, "%04d-%02d-%02dT00:00:00Z", year, month, day);
      return 1;
    }
  }
  return 0;
}

// s as JSON string content (no quotes) into out, cut at a character
// boundary if cap is short. Invalid UTF-8 becomes '?': email often carries
// raw Latin-1, and a 422 on content would fail the input on every retry.
static void classifyJsonEsc(const char *s, char *out, usize cap) {
  static const char hex[] = "0123456789abcdef";
  const u8 *p = (const u8 *)s;
  usize o = 0;
  while (*p && o + 7 <= cap) { // 7: longest write (\u00XX) + NUL
    u8 c = *p;
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = (char)c;
      p++;
    } else if (c == '\n' || c == '\r' || c == '\t') {
      out[o++] = '\\';
      out[o++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
      p++;
    } else if (c < 0x20) {
      memcpy(out + o, "\\u00", 4);
      out[o + 4] = hex[c >> 4];
      out[o + 5] = hex[c & 15];
      o += 6;
      p++;
    } else if (c < 0x80) {
      out[o++] = (char)c;
      p++;
    } else {
      // Well-formed UTF-8 only (RFC 3629): no overlongs, surrogates or
      // code points past U+10FFFF. A NUL fails the continuation check, so
      // this never reads past the terminator.
      usize n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
      u8 lo = c == 0xe0 ? 0xa0 : c == 0xf0 ? 0x90 : 0x80;
      u8 hi = c == 0xed ? 0x9f : c == 0xf4 ? 0x8f : 0xbf;
      int valid = c >= 0xc2 && c <= 0xf4 && p[1] >= lo && p[1] <= hi;
      for (usize i = 2; i < n && valid; i++) valid = (p[i] & 0xc0) == 0x80;
      if (valid) {
        memcpy(out + o, p, n);
        o += n;
        p += n;
      } else {
        out[o++] = '?';
        p++;
      }
    }
  }
  out[o] = 0;
}

static size_t classifySink(char *ptr, size_t size, size_t n, void *ud) {
  (void)ud;
  usize want = size * n;
  if (classify_resp_len + want > CLASSIFY_RESP_CAP) return 0; // fails the transfer
  memcpy(classify_resp + classify_resp_len, ptr, want);
  classify_resp_len += want;
  classify_resp[classify_resp_len] = 0;
  return want;
}

int classifyInit(void) {
  const char *key = configEnv("OPENCODE_API_KEY", "");
  if (!*key) {
    configLog("OPENCODE_API_KEY is not set");
    return -1;
  }
  char auth[512];
  if (snprintf(auth, sizeof auth, "Authorization: Bearer %s", key) >= (int)sizeof auth) {
    configLog("OPENCODE_API_KEY is too long");
    return -1;
  }
  // "Expect:" drops curl's 100-continue wait on bodies over 1 KiB.
  const char *lines[] = {auth, "Content-Type: application/json", "Expect:"};
  CURL *ch = curl_easy_init();
  struct curl_slist *hdrs = NULL;
  int ok = ch != NULL;
  for (usize i = 0; i < sizeof lines / sizeof lines[0] && ok; i++) {
    struct curl_slist *next = curl_slist_append(hdrs, lines[i]);
    ok = next != NULL;
    if (next) hdrs = next;
  }
  if (!ok) {
    configLog("zen: curl init failed");
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(ch);
    return -1;
  }
  curl_easy_setopt(ch, CURLOPT_URL, configEnv("GENDA_ZEN_URL", "https://opencode.ai/zen/v1/systemone"));
  curl_easy_setopt(ch, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(ch, CURLOPT_POSTFIELDS, classify_req); // size set per call
  curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, classifySink);
  curl_easy_setopt(ch, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(ch, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt(ch, CURLOPT_NOSIGNAL, 1L); // timeouts without SIGALRM: threads
  classifyJsonEsc(configEnv("GENDA_ZEN_MODEL", "jev-1.13-free"), classify_model, sizeof classify_model);
  // Before any thread runs (tests re-init to change env): no lock needed.
  curl_easy_cleanup(classify_curl);
  curl_slist_free_all(classify_hdrs);
  classify_curl = ch;
  classify_hdrs = hdrs;
  return 0;
}

// Build the request for in (date "" = no is_deadline question) and POST it.
// Returns 0 when Zen answered 200 with its reply in classify_resp; -1 logged.
static int classifyPost(const Input *in, const char *date) {
  classifyJsonEsc(in->source, classify_esc.source, sizeof classify_esc.source);
  classifyJsonEsc(in->app, classify_esc.app, sizeof classify_esc.app);
  classifyJsonEsc(in->from, classify_esc.from, sizeof classify_esc.from);
  classifyJsonEsc(in->title, classify_esc.title, sizeof classify_esc.title);
  classifyJsonEsc(in->text, classify_esc.text, sizeof classify_esc.text);
  char deadline_q[1024] = ""; // date is our own ASCII: no escaping
  if (date[0]) snprintf(deadline_q, sizeof deadline_q, CLASSIFY_DEADLINE_FMT, date);
  int n = snprintf(classify_req, sizeof classify_req, CLASSIFY_REQ_FMT, classify_model,
                   classify_esc.source, classify_esc.app, classify_esc.from, classify_esc.title,
                   classify_esc.text, deadline_q);
  if (n < 0 || (usize)n >= sizeof classify_req) { // unreachable by sizing
    configLog("zen: request over %zu bytes", sizeof classify_req);
    return -1;
  }
  curl_easy_setopt(classify_curl, CURLOPT_POSTFIELDSIZE, (long)n);
  classify_resp_len = 0;
  classify_resp[0] = 0;
  CURLcode rc = curl_easy_perform(classify_curl);
  if (rc == CURLE_WRITE_ERROR) {
    configLog("zen: reply over %d bytes", CLASSIFY_RESP_CAP);
    return -1;
  }
  if (rc != CURLE_OK) {
    configLog("zen: %s", curl_easy_strerror(rc));
    return -1;
  }
  long code = 0;
  curl_easy_getinfo(classify_curl, CURLINFO_RESPONSE_CODE, &code);
  if (code != 200) {
    configLog("zen http %ld: %.200s", code, classify_resp);
    return -1;
  }
  return 0;
}

// Value token of key in object token obj; n (the nil token, type
// JSMN_UNDEFINED) when obj is not an object or has no such key, so lookups
// chain and only the final type check fails.
static int classifyJsonGet(const jsmntok_t *t, int n, int obj, const char *key) {
  if (t[obj].type != JSMN_OBJECT) return n;
  usize klen = strlen(key);
  for (int i = obj + 1; i + 1 < n; i++) // keys are the children of obj
    if (t[i].parent == obj && (usize)(t[i].end - t[i].start) == klen &&
        !memcmp(classify_resp + t[i].start, key, klen))
      return i + 1;
  return n;
}

// Token as a probability: a number spanning the whole token, in [0,1].
static int classifyJsonProb(jsmntok_t t, f64 *out) {
  char num[64]; // longest float repr is ~24 chars
  usize len = (usize)(t.end - t.start);
  if (t.type != JSMN_PRIMITIVE || len == 0 || len >= sizeof num) return -1;
  memcpy(num, classify_resp + t.start, len);
  num[len] = 0;
  char *end = NULL;
  f64 v = strtod(num, &end);
  if (end != num + len || !(v >= 0 && v <= 1)) return -1; // NaN fails too
  *out = v;
  return 0;
}

// The Zen reply boundary, the one place that trusts nothing Zen sent: kind,
// confidence = 1 - P(none) and, when has_date, the is_deadline noul.
// Returns 0 ok, -1 logged.
static int classifyParseReply(int has_date, Classified *c, f64 *deadline_p) {
  jsmn_parser p;
  jsmn_init(&p);
  int n = jsmn_parse(&p, classify_resp, classify_resp_len, classify_tok, CLASSIFY_TOK_CAP);
  if (n < 1 || classify_tok[0].type != JSMN_OBJECT) {
    configLog("zen reply: not a JSON object (%d): %.200s", n, classify_resp);
    return -1;
  }
  const jsmntok_t *t = classify_tok;
  classify_tok[n] = (jsmntok_t){0}; // nil: every missing key resolves here
  int answers = classifyJsonGet(t, n, 0, "answers");
  int kind = classifyJsonGet(t, n, answers, "kind");
  jsmntok_t choice = t[classifyJsonGet(t, n, kind, "choice")];
  jsmntok_t none = t[classifyJsonGet(t, n, classifyJsonGet(t, n, kind, "probabilities"), "none")];
  static const char *const names[] = {
      [KIND_NONE] = "none", [KIND_APPOINTMENT] = "appointment", [KIND_OBLIGATION] = "obligation"};
  int found = 0;
  usize clen = (usize)(choice.end - choice.start);
  for (usize k = 0; k < sizeof names / sizeof names[0] && choice.type == JSMN_STRING; k++)
    if (strlen(names[k]) == clen && !memcmp(classify_resp + choice.start, names[k], clen)) {
      c->kind = (EventKind)k;
      found = 1;
    }
  f64 p_none = 0;
  if (!found || classifyJsonProb(none, &p_none)) {
    configLog("zen reply: bad kind answer: %.200s", classify_resp);
    return -1;
  }
  c->confidence = 1 - p_none;
  if (has_date) {
    jsmntok_t noul = t[classifyJsonGet(t, n, classifyJsonGet(t, n, answers, "is_deadline"), "noul")];
    if (classifyJsonProb(noul, deadline_p)) {
      configLog("zen reply: bad is_deadline answer: %.200s", classify_resp);
      return -1;
    }
  }
  return 0;
}

Classified classifyInput(const Input *in) {
  Classified c = {0};
  char date[64] = "";
  int has_date = classifyScanIsoDate(in->title, date, sizeof date) ||
                 classifyScanIsoDate(in->text, date, sizeof date);
  f64 deadline_p = 0;
  pthread_mutex_lock(&classify_mu);
  int ok = !classifyPost(in, date) && !classifyParseReply(has_date, &c, &deadline_p);
  pthread_mutex_unlock(&classify_mu);
  if (!ok) return (Classified){0}; // a half-parsed reply leaves nothing
  c.ok = 1;
  c.is_event = c.confidence >= 0.3 && c.kind != KIND_NONE;
  snprintf(c.title, sizeof c.title, "%s", in->title[0] ? in->title : in->text);
  if (has_date) snprintf(deadline_p >= 0.5 ? c.deadline : c.starts_at, sizeof c.deadline, "%s", date);
  return c;
}
