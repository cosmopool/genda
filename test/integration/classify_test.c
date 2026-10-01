// classify module tests: heuristic mapping is pure — no db, no network.
#include "../../src/classify.h"
#include "../../src/common.h"
#include "../vendor/greatest.h"

#include <stdio.h>
#include <string.h>

static void fillInput(Input *in, const char *title, const char *text) {
  memset(in, 0, sizeof *in);
  snprintf(in->title, sizeof in->title, "%s", title);
  snprintf(in->text, sizeof in->text, "%s", text);
}

TEST appointmentKeywordWithDate(void) {
  Input in;
  fillInput(&in, "Team meeting", "see you 2026-10-01T10:00");
  Classified c;
  ASSERT_EQ(0, classifyInput(&in, &c));
  ASSERT_STR_EQ("appointment", c.kind);
  ASSERT(c.confidence >= 0.3);
  ASSERT_STR_EQ("2026-10-01T10:00:00Z", c.starts_at);
  PASS();
}

TEST obligationKeywordWithDeadline(void) {
  Input in;
  fillInput(&in, "Invoice due", "please pay by 2026-10-05");
  Classified c;
  ASSERT_EQ(0, classifyInput(&in, &c));
  ASSERT_STR_EQ("obligation", c.kind);
  ASSERT(c.confidence >= 0.3);
  ASSERT_STR_EQ("2026-10-05T00:00:00Z", c.deadline);
  ASSERT(c.starts_at[0] == 0);
  PASS();
}

TEST noiseIsNone(void) {
  Input in;
  fillInput(&in, "meme of the day", "haha look at this");
  Classified c;
  ASSERT_EQ(0, classifyInput(&in, &c));
  ASSERT_STR_EQ("none", c.kind);
  ASSERT(c.confidence < 0.3);
  PASS();
}

TEST emptyTitleFallsBackToText(void) {
  Input in;
  fillInput(&in, "", "dentist visit tomorrow");
  Classified c;
  ASSERT_EQ(0, classifyInput(&in, &c));
  ASSERT_STR_EQ("appointment", c.kind);
  ASSERT_STR_EQ("dentist visit tomorrow", c.title);
  PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_TEST(appointmentKeywordWithDate);
  RUN_TEST(obligationKeywordWithDeadline);
  RUN_TEST(noiseIsNone);
  RUN_TEST(emptyTitleFallsBackToText);
  GREATEST_MAIN_END();
  (void)argc;
  (void)argv;
}
