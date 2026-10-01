// Minimal MessagePack subset: nil bool uint int float str bin map array.
#include "msgpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mpReserve(MpWriter *w, size_t extra) {
  if (w->len + extra <= w->cap) return;
  size_t ncap = w->cap ? w->cap * 2 : 256;
  while (ncap < w->len + extra) ncap *= 2;
  w->p = realloc(w->p, ncap);
  w->cap = ncap;
}

static void mpB(MpWriter *w, unsigned char b) {
  mpReserve(w, 1);
  w->p[w->len++] = b;
}

void mpMap(MpWriter *w, unsigned long n) {
  if (n < 16) return mpB(w, (unsigned char)(0x80 | n));
  mpReserve(w, 3);
  w->p[w->len++] = 0xde;
  w->p[w->len++] = (unsigned char)(n >> 8);
  w->p[w->len++] = (unsigned char)n;
}

void mpArr(MpWriter *w, unsigned long n) {
  if (n < 16) return mpB(w, (unsigned char)(0x90 | n));
  mpReserve(w, 3);
  w->p[w->len++] = 0xdc;
  w->p[w->len++] = (unsigned char)(n >> 8);
  w->p[w->len++] = (unsigned char)n;
}

void mpStr(MpWriter *w, const char *s) {
  size_t n = strlen(s);
  if (n < 32) {
    mpB(w, (unsigned char)(0xa0 | n));
  } else if (n < 256) {
    mpReserve(w, 2);
    w->p[w->len++] = 0xd9;
    w->p[w->len++] = (unsigned char)n;
  } else {
    mpReserve(w, 3);
    w->p[w->len++] = 0xda;
    w->p[w->len++] = (unsigned char)(n >> 8);
    w->p[w->len++] = (unsigned char)n;
  }
  mpReserve(w, n);
  memcpy(w->p + w->len, s, n);
  w->len += n;
}

void mpU64(MpWriter *w, unsigned long long v) {
  if (v < 128) return mpB(w, (unsigned char)v);
  mpReserve(w, 9);
  w->p[w->len++] = 0xcf;
  for (int i = 7; i >= 0; i--) w->p[w->len++] = (unsigned char)(v >> (i * 8));
}

void mpF64(MpWriter *w, double v) {
  mpReserve(w, 9);
  w->p[w->len++] = 0xcb;
  unsigned long long u;
  memcpy(&u, &v, 8);
  for (int i = 7; i >= 0; i--) w->p[w->len++] = (unsigned char)(u >> (i * 8));
}

static int mpByte(MpReader *r, unsigned char *out) {
  if (r->p >= r->end) return -1;
  *out = *r->p++;
  return 0;
}

// If next value is map/array header, return length. Else -1.
long mpHdrLen(MpReader *r, unsigned char fix, unsigned char b16, unsigned char b32) {
  unsigned char b;
  if (mpByte(r, &b)) return -1;
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

int mpSkip(MpReader *r);

static int mpSkipN(MpReader *r, long n) {
  if (r->end - r->p < n) return -1;
  r->p += n;
  return 0;
}

int mpSkip(MpReader *r) {
  if (r->p >= r->end) return -1;
  unsigned char b = *r->p;
  if (b < 0x80 || (b >= 0xe0)) {
    r->p++;
    return 0;
  }
  if ((b & 0xe0) == 0xa0) { // fixstr
    long n = b & 0x1f;
    r->p++;
    return mpSkipN(r, n);
  }
  if ((b & 0xf0) == 0x80) { // fixmap
    long n = b & 0x0f;
    r->p++;
    for (long i = 0; i < n; i++)
      if (mpSkip(r) || mpSkip(r)) return -1;
    return 0;
  }
  if ((b & 0xf0) == 0x90) { // fixarray
    long n = b & 0x0f;
    r->p++;
    for (long i = 0; i < n; i++)
      if (mpSkip(r)) return -1;
    return 0;
  }
  r->p++;
  switch (b) {
  case 0xc0: // nil
  case 0xc2: // false
  case 0xc3: // true
    return 0;
  case 0xcc:
    return mpSkipN(r, 1);
  case 0xcd:
    return mpSkipN(r, 2);
  case 0xce:
    return mpSkipN(r, 4);
  case 0xcf:
  case 0xd3:
  case 0xcb:
    return mpSkipN(r, 8);
  case 0xd0:
    return mpSkipN(r, 1);
  case 0xd1:
    return mpSkipN(r, 2);
  case 0xd2:
    return mpSkipN(r, 4);
  case 0xca:
    return mpSkipN(r, 4);
  case 0xd9: {
    unsigned char n;
    if (mpByte(r, &n)) return -1;
    return mpSkipN(r, n);
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
    return mpSkipN(r, n);
  }
  case 0xdc:
  case 0xdd: {
    MpReader t = *r;
    t.p--;
    long n = mpHdrLen(&t, 0, 0xdc, 0xdd);
    if (n < 0) return -1;
    *r = t;
    for (long i = 0; i < n; i++)
      if (mpSkip(r)) return -1;
    return 0;
  }
  case 0xde:
  case 0xdf: {
    MpReader t = *r;
    t.p--;
    long n = mpHdrLen(&t, 0, 0xde, 0xdf);
    if (n < 0) return -1;
    *r = t;
    for (long i = 0; i < n; i++)
      if (mpSkip(r) || mpSkip(r)) return -1;
    return 0;
  }
  case 0xc4:
  case 0xc5:
  case 0xc6: {
    // bin8/16/32
    long n = -1;
    if (b == 0xc4) {
      unsigned char m;
      if (mpByte(r, &m)) return -1;
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
    return mpSkipN(r, n);
  }
  default:
    return -1;
  }
}

// Read next value as NUL-terminated string (str/bin families) or scalar
// rendered as text (uint/int/float/bool/nil->""). Returns malloc'd buf.
char *mpStrval(MpReader *r) {
  if (r->p >= r->end) return NULL;
  unsigned char b = *r->p;
  long n;
  if ((b & 0xe0) == 0xa0 || b == 0xd9) {
    MpReader t = *r;
    t.p++;
    if ((b & 0xe0) == 0xa0) {
      n = b & 0x1f;
    } else {
      unsigned char m;
      if (mpByte(&t, &m)) return NULL;
      n = m;
    }
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
      if (mpByte(r, &m)) return NULL;
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
      if (mpByte(r, &m)) return NULL;
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
      if (mpByte(r, (unsigned char *)&m)) return NULL;
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
