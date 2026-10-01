// Minimal MessagePack subset: nil bool uint int float str bin map array.
#ifndef GENDA_MSGPACK_H
#define GENDA_MSGPACK_H

#include "core.h"

typedef struct {
  u8 *p;
  usize len, cap;
} MpWriter;

typedef struct {
  const u8 *p, *end;
} MpReader;

void mpReserve(MpWriter *w, usize extra);
void mpMap(MpWriter *w, u64 n);
void mpArr(MpWriter *w, u64 n);
void mpStr(MpWriter *w, const char *s);
void mpU64(MpWriter *w, u64 v);
void mpF64(MpWriter *w, f64 v);
// If next value is a map/array header, consume it and return length. Else -1.
i64 mpHdrLen(MpReader *r, u8 fix, u8 b16, u8 b32);
// Skip one whole value (nested included). Returns 0 ok.
int mpSkip(MpReader *r);
// Next value as NUL-terminated text into out (str/bin as-is, scalars
// rendered), cut to cap-1 bytes; the whole value is consumed either way.
// cap >= 1. Returns 0 ok, -1 on truncated or non-scalar input.
int mpStrVal(MpReader *r, char *out, usize cap);

#endif
