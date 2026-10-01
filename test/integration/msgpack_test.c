// msgpack module tests: pack/parse round-trips and hostile input.
#include "../../src/msgpack.h"
#include "../vendor/greatest.h"

#include <stdlib.h>
#include <string.h>

TEST roundTripScalars(void) {
  MpWriter w = {0};
  mpMap(&w, 4);
  mpStr(&w, "title");
  mpStr(&w, "Dentist 2026-10-01");
  mpStr(&w, "raw_id");
  mpU64(&w, 42);
  mpStr(&w, "confidence");
  mpF64(&w, 0.9);
  mpStr(&w, "tags");
  mpArr(&w, 2);
  mpStr(&w, "a");
  mpStr(&w, "b");

  MpReader r = {w.p, w.p + w.len};
  ASSERT_EQ(4, mpHdrLen(&r, 0x80, 0xde, 0xdf));
  char *k = mpStrval(&r);
  ASSERT_STR_EQ("title", k);
  free(k);
  char *v = mpStrval(&r);
  ASSERT_STR_EQ("Dentist 2026-10-01", v);
  free(v);
  k = mpStrval(&r);
  ASSERT_STR_EQ("raw_id", k);
  free(k);
  v = mpStrval(&r);
  ASSERT_STR_EQ("42", v);
  free(v);
  k = mpStrval(&r);
  ASSERT_STR_EQ("confidence", k);
  free(k);
  v = mpStrval(&r);
  ASSERT_STR_EQ("0.9", v);
  free(v);
  k = mpStrval(&r);
  ASSERT_STR_EQ("tags", k);
  free(k);
  ASSERT_EQ(2, mpHdrLen(&r, 0x90, 0xdc, 0xdd));
  v = mpStrval(&r);
  ASSERT_STR_EQ("a", v);
  free(v);
  v = mpStrval(&r);
  ASSERT_STR_EQ("b", v);
  free(v);
  ASSERT(r.p == r.end);
  free(w.p);
  PASS();
}

TEST longStringUsesStr8(void) {
  char big[41];
  memset(big, 'x', 40);
  big[40] = 0;
  MpWriter w = {0};
  mpStr(&w, big);
  ASSERT(w.len == 42); // 0xd9 hdr + 40 bytes
  MpReader r = {w.p, w.p + w.len};
  char *v = mpStrval(&r);
  ASSERT_STR_EQ(big, v);
  free(v);
  free(w.p);
  PASS();
}

// Every str8 length must decode exactly (length byte read as unsigned).
TEST str8AllLengthsRoundTrip(void) {
  char big[256];
  for (int n = 32; n < 256; n++) {
    memset(big, 'y', (size_t)n);
    big[n] = 0;
    MpWriter w = {0};
    mpStr(&w, big);
    ASSERT_EQ(0xd9, w.p[0]);
    MpReader r = {w.p, w.p + w.len};
    char *v = mpStrval(&r);
    ASSERT(v != NULL);
    ASSERT_STR_EQ(big, v);
    ASSERT(r.p == r.end);
    free(v);
    free(w.p);
  }
  PASS();
}

TEST skipNestedValues(void) {
  MpWriter w = {0};
  mpMap(&w, 2);
  mpStr(&w, "arr");
  mpArr(&w, 2);
  mpU64(&w, 1);
  mpMap(&w, 1);
  mpStr(&w, "k");
  mpStr(&w, "v");
  mpStr(&w, "tail");
  mpU64(&w, 7);
  MpReader r = {w.p, w.p + w.len};
  ASSERT_EQ(2, mpHdrLen(&r, 0x80, 0xde, 0xdf));
  char *k = mpStrval(&r);
  ASSERT_STR_EQ("arr", k);
  free(k);
  ASSERT_EQ(0, mpSkip(&r)); // whole array incl. nested map
  k = mpStrval(&r);
  ASSERT_STR_EQ("tail", k);
  free(k);
  char *v = mpStrval(&r);
  ASSERT_STR_EQ("7", v);
  free(v);
  ASSERT(r.p == r.end);
  free(w.p);
  PASS();
}

TEST truncatedInputFails(void) {
  unsigned char cut[] = {0xa5, 'h', 'i'}; // fixstr(5) with 2 bytes
  MpReader r = {cut, cut + sizeof cut};
  ASSERT(mpStrval(&r) == NULL);
  unsigned char notmap[] = {0x01};
  MpReader r2 = {notmap, notmap + 1};
  ASSERT_EQ(-1, mpHdrLen(&r2, 0x80, 0xde, 0xdf));
  MpReader r3 = {notmap, notmap + 0};
  ASSERT_EQ(-1, mpSkip(&r3));
  unsigned char scalar[] = {0xc0, 0xc3, 0x2a};
  MpReader r4 = {scalar, scalar + sizeof scalar};
  char *v = mpStrval(&r4);
  ASSERT_STR_EQ("", v);
  free(v);
  v = mpStrval(&r4);
  ASSERT_STR_EQ("true", v);
  free(v);
  v = mpStrval(&r4);
  ASSERT_STR_EQ("42", v);
  free(v);
  PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_TEST(roundTripScalars);
  RUN_TEST(longStringUsesStr8);
  RUN_TEST(str8AllLengthsRoundTrip);
  RUN_TEST(skipNestedValues);
  RUN_TEST(truncatedInputFails);
  GREATEST_MAIN_END();
  (void)argc;
  (void)argv;
}
