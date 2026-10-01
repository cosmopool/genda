// Minimal MessagePack subset: nil bool uint int float str bin map array.
#ifndef GENDA_MSGPACK_H
#define GENDA_MSGPACK_H

#include <stddef.h>

typedef struct {
  unsigned char *p;
  size_t len, cap;
} MpWriter;

typedef struct {
  const unsigned char *p, *end;
} MpReader;

void mpReserve(MpWriter *w, size_t extra);
void mpMap(MpWriter *w, unsigned long n);
void mpArr(MpWriter *w, unsigned long n);
void mpStr(MpWriter *w, const char *s);
void mpU64(MpWriter *w, unsigned long long v);
void mpF64(MpWriter *w, double v);
// If next value is a map/array header, consume it and return length. Else -1.
long mpHdrLen(MpReader *r, unsigned char fix, unsigned char b16, unsigned char b32);
// Skip one whole value (nested included). Returns 0 ok.
int mpSkip(MpReader *r);
// Next value as NUL-terminated text (str/bin as-is, scalars rendered).
// Returns malloc'd buf, NULL on truncated input.
char *mpStrval(MpReader *r);

#endif
