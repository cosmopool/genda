// classify module tests: classifyInit/classifyInput against the fake Zen
// server (zen_fake.h). No real network.
#include "../../src/classify.h"
#include "../../src/common.h"
#include "../vendor/greatest.h"
#include "zen_fake.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

static char body[ZEN_FAKE_BODY_CAP + 1];

static void testSetup(void *arg) {
  (void)arg;
  zenFakeMode(ZEN_FAKE_OK);
}

TEST appointmentGetsStartsAt(void) {
  Input in = {.source = "notif", .title = "Dentist 2026-10-01T10:00", .text = "confirming your visit"};
  Classified c = classifyInput(&in);
  ASSERT_EQ(1, c.ok);
  ASSERT_EQ(KIND_APPOINTMENT, c.kind);
  ASSERT_IN_RANGE(0.95, c.confidence, 1e-9); // 1 - P(none)
  ASSERT_EQ(1, c.is_event);
  ASSERT_STR_EQ("Dentist 2026-10-01T10:00", c.title);
  ASSERT_STR_EQ("2026-10-01T10:00:00Z", c.starts_at);
  ASSERT_STR_EQ("", c.deadline);
  ASSERT_STR_EQ("", c.location);
  PASS();
}

TEST obligationGetsDeadline(void) {
  Input in = {.source = "email", .title = "", .text = "invoice due 2026-10-15"};
  Classified c = classifyInput(&in);
  ASSERT_EQ(1, c.ok);
  ASSERT_EQ(KIND_OBLIGATION, c.kind);
  ASSERT_EQ(1, c.is_event);
  ASSERT_STR_EQ("invoice due 2026-10-15", c.title); // no title: text
  ASSERT_STR_EQ("2026-10-15T00:00:00Z", c.deadline);
  ASSERT_STR_EQ("", c.starts_at);
  PASS();
}

TEST noiseIsNotAnEvent(void) {
  Input in = {.source = "notif", .title = "meme of the day", .text = "haha look at this"};
  Classified c = classifyInput(&in);
  ASSERT_EQ(1, c.ok);
  ASSERT_EQ(KIND_NONE, c.kind);
  ASSERT_EQ(0, c.is_event);
  zenFakeLastBody(body, sizeof body);
  ASSERT(!strstr(body, "is_deadline")); // no date found: no deadline question
  PASS();
}

TEST failuresLeaveZero(void) {
  Input in = {.source = "notif", .title = "Dentist 2026-10-01T10:00", .text = "confirming"};
  ZenFakeMode modes[] = {ZEN_FAKE_FAIL_500, ZEN_FAKE_GARBAGE, ZEN_FAKE_TRUNCATED, ZEN_FAKE_BAD_PROB};
  for (usize i = 0; i < sizeof modes / sizeof modes[0]; i++) {
    zenFakeMode(modes[i]);
    Classified c = classifyInput(&in);
    ASSERT_EQ_FMT(0, c.ok, "%d");
    ASSERT_EQ(0, c.is_event);
    ASSERT_EQ(KIND_NONE, c.kind);
    ASSERT_STR_EQ("", c.title);
  }
  zenFakeMode(ZEN_FAKE_OK);
  setenv("OPENCODE_API_KEY", "wrong-key", 1); // fake answers 401
  int init = classifyInit();
  Classified c = classifyInput(&in);
  setenv("OPENCODE_API_KEY", "test-key", 1);
  int reinit = classifyInit();
  ASSERT_EQ(0, init);
  ASSERT_EQ(0, reinit);
  ASSERT_EQ(0, c.ok);
  ASSERT_EQ(1, classifyInput(&in).ok); // and recovers
  PASS();
}

TEST modelComesFromEnv(void) {
  Input in = {.source = "notif", .title = "meme", .text = "haha"};
  classifyInput(&in);
  zenFakeLastBody(body, sizeof body);
  ASSERT(strstr(body, "\"jev-1.13-free\"")); // default
  setenv("GENDA_ZEN_MODEL", "jev-test-model-7", 1);
  int init = classifyInit();
  Classified c = classifyInput(&in);
  zenFakeLastBody(body, sizeof body);
  unsetenv("GENDA_ZEN_MODEL");
  int reinit = classifyInit();
  ASSERT_EQ(0, init);
  ASSERT_EQ(0, reinit);
  ASSERT_EQ(1, c.ok);
  ASSERT(strstr(body, "\"jev-test-model-7\""));
  PASS();
}

// Quotes, backslash, newline, a control byte, valid UTF-8 and broken UTF-8
// (a stray 0xff, a sequence cut short by a quote) still make valid JSON.
TEST hostileTextIsValidJson(void) {
  const char *title = "Say \"hi\" \\ back\nnow\x01 caf\xc3\xa9 bad\xff\xe2\x82\" end";
  Input in = {.source = "email", .text = "dentist 2026-10-01T10:00"};
  snprintf(in.title, sizeof in.title, "%s", title);
  Classified c = classifyInput(&in);
  ASSERT_EQ(1, c.ok); // the fake answers 422 to invalid JSON
  ASSERT_STR_EQ(title, c.title); // the stored title stays untouched
  zenFakeLastBody(body, sizeof body);
  static jsmntok_t tok[128];
  jsmn_parser p;
  jsmn_init(&p);
  int n = jsmn_parse(&p, body, strlen(body), tok, sizeof tok / sizeof tok[0]);
  ASSERT(n > 0);
  ASSERT_EQ(JSMN_OBJECT, tok[0].type);
  ASSERT_EQ((int)strlen(body), tok[0].end); // the whole body is one object
  for (const char *s = body; *s; s++) ASSERT((u8)*s >= 0x20); // controls escaped
  ASSERT(strstr(body, "caf\xc3\xa9"));                      // valid UTF-8 kept
  ASSERT(!strchr(body, '\xff'));
  ASSERT(!strstr(body, "\xe2\x82"));
  PASS();
}

TEST initFailsWithoutKey(void) {
  unsetenv("OPENCODE_API_KEY");
  int rc = classifyInit();
  setenv("OPENCODE_API_KEY", "test-key", 1);
  ASSERT_EQ(-1, rc);
  Input in = {.source = "notif", .title = "meme", .text = "haha"};
  ASSERT_EQ(1, classifyInput(&in).ok); // the working handle is kept
  PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  curl_global_init(CURL_GLOBAL_ALL);
  if (zenFakeStart() || classifyInit()) {
    fprintf(stderr, "classify setup failed\n");
    return 1;
  }
  SET_SETUP(testSetup, NULL);
  RUN_TEST(appointmentGetsStartsAt);
  RUN_TEST(obligationGetsDeadline);
  RUN_TEST(noiseIsNotAnEvent);
  RUN_TEST(failuresLeaveZero);
  RUN_TEST(modelComesFromEnv);
  RUN_TEST(hostileTextIsValidJson);
  RUN_TEST(initFailsWithoutKey);
  GREATEST_MAIN_END();
  (void)argc;
  (void)argv;
}
