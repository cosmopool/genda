// Input classification into calendar events.
// Keyword heuristic for now (TODO: LLM). Stores nothing; sets is_event when
// an events row should be written.
#include "classify.h"

#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// Lowercase src into dst (cap >= 1), cut to cap-1. Returns bytes written.
static size_t classifyLowerCopy(const char *src, char *dst, size_t cap) {
  size_t i = 0;
  for (; src[i] && i + 1 < cap; i++) dst[i] = (char)tolower((unsigned char)src[i]);
  dst[i] = 0;
  return i;
}

static int classifyContainsAny(const char *hay, const char *words[]) {
  for (int i = 0; words[i]; i++)
    if (strstr(hay, words[i])) return 1;
  return 0;
}

// First YYYY-MM-DD([T ]HH:MM) occurrence -> out. Returns 1 found.
static int classifyScanIsoDate(const char *s, char *out, size_t cap) {
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

// c arrives zeroed.
static void classifyHeuristic(const Input *in, Classified *c) {
  snprintf(c->title, sizeof c->title, "%s", in->title[0] ? in->title : in->text);
  // lowercased "title text"; fits whole: title < sizeof title, text < sizeof text
  char hay[sizeof in->title + sizeof in->text];
  size_t n = classifyLowerCopy(in->title, hay, sizeof hay);
  hay[n++] = ' ';
  classifyLowerCopy(in->text, hay + n, sizeof hay - n);
  static const char *appt[] = {"meeting",  "appointment", "call",     "interview", "dentist",
                               "doctor",   "flight",      "booking",  "reservation", "conference",
                               "webinar",  "standup",     "ceremony", "party",     NULL};
  static const char *oblg[] = {"deadline", "due",     "invoice", "bill",   "pay",
                               "rent",     "tax",     "submit",  "renew",  "expir",
                               "overdue",  "payment", "fine",    NULL};
  if (classifyContainsAny(hay, appt)) {
    c->kind = KIND_APPOINTMENT;
    c->confidence = 0.45;
  } else if (classifyContainsAny(hay, oblg)) {
    c->kind = KIND_OBLIGATION;
    c->confidence = 0.45;
  } else {
    c->kind = KIND_NONE;
    return;
  }
  char dt[64] = "";
  if (classifyScanIsoDate(in->title, dt, sizeof dt) || classifyScanIsoDate(in->text, dt, sizeof dt)) {
    if (c->kind == KIND_OBLIGATION && (strstr(hay, "deadline") || strstr(hay, "due")))
      snprintf(c->deadline, sizeof c->deadline, "%s", dt);
    else
      snprintf(c->starts_at, sizeof c->starts_at, "%s", dt);
  }
}

// TODO: real classifier (LLM) lands here. Mock = keyword heuristic only.
Classified classifyInput(const Input *in) {
  Classified c = {0};
  classifyHeuristic(in, &c);
  c.is_event = c.confidence >= 0.3 && c.kind != KIND_NONE;
  return c;
}
