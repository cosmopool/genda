// Minimal MessagePack subset: nil bool uint int float str bin map array.
#ifndef GENDA_MSGPACK_H
#define GENDA_MSGPACK_H

#include <stddef.h>

typedef struct {
  unsigned char *p;
  size_t len, cap;
} MPW;

typedef struct {
  const unsigned char *p, *end;
} MPR;

void mp_reserve(MPW *w, size_t extra);
void mp_map(MPW *w, unsigned long n);
void mp_arr(MPW *w, unsigned long n);
void mp_str(MPW *w, const char *s);
void mp_u64(MPW *w, unsigned long long v);
void mp_f64(MPW *w, double v);
// If next value is a map/array header, consume it and return length. Else -1.
long mp_hdr_len(MPR *r, unsigned char fix, unsigned char b16, unsigned char b32);
// Skip one whole value (nested included). Returns 0 ok.
int mp_skip(MPR *r);
// Next value as NUL-terminated text (str/bin as-is, scalars rendered).
// Returns malloc'd buf, NULL on truncated input.
char *mp_strval(MPR *r);

#endif
