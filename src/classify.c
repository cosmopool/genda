// Input classification into calendar events.
// Keyword heuristic for now (TODO: LLM). Stores nothing; sets is_event when
// an events row should be written.
#include "classify.h"

#include "common.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void classifyLowerCopy(const char *src, char *dst, size_t cap) {
  size_t i = 0;
  for (; src[i] && i + 1 < cap; i++) dst[i] = (char)tolower((unsigned char)src[i]);
  dst[i] = 0;
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
    int Y, M, D, h = -1, m = -1;
    if (sscanf(s, "%4d-%2d-%2d", &Y, &M, &D) == 3 && Y >= 2020 && Y <= 2100 && M >= 1 &&
        M <= 12 && D >= 1 && D <= 31) {
      if ((s[10] == 'T' || s[10] == ' ') && sscanf(s + 11, "%2d:%2d", &h, &m) == 2 && h >= 0 &&
          h < 24 && m >= 0 && m < 60)
        snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:00Z", Y, M, D, h, m);
      else
        snprintf(out, cap, "%04d-%02d-%02dT00:00:00Z", Y, M, D);
      return 1;
    }
  }
  return 0;
}

// c arrives zeroed.
static void classifyHeuristic(const Input *in, Classified *c) {
  snprintf(c->title, sizeof c->title, "%s", in->title[0] ? in->title : in->text);
  char hay[4608];
  char tmp[4608];
  snprintf(tmp, sizeof tmp, "%s %s", in->title, in->text);
  classifyLowerCopy(tmp, hay, sizeof hay);
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
