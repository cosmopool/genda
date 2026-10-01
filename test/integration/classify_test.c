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
  Classified c = classifyInput(&in);
  ASSERT_EQ(KIND_APPOINTMENT, c.kind);
  ASSERT(c.is_event);
  ASSERT_STR_EQ("2026-10-01T10:00:00Z", c.starts_at);
  PASS();
}

TEST obligationKeywordWithDeadline(void) {
  Input in;
  fillInput(&in, "Invoice due", "please pay by 2026-10-05");
  Classified c = classifyInput(&in);
  ASSERT_EQ(KIND_OBLIGATION, c.kind);
  ASSERT(c.is_event);
  ASSERT_STR_EQ("2026-10-05T00:00:00Z", c.deadline);
  ASSERT(c.starts_at[0] == 0);
  PASS();
}

TEST noiseIsNone(void) {
  Input in;
  fillInput(&in, "meme of the day", "haha look at this");
  Classified c = classifyInput(&in);
  ASSERT_EQ(KIND_NONE, c.kind);
  ASSERT(!c.is_event);
  PASS();
}

TEST emptyTitleFallsBackToText(void) {
  Input in;
  fillInput(&in, "", "dentist visit tomorrow");
  Classified c = classifyInput(&in);
  ASSERT_EQ(KIND_APPOINTMENT, c.kind);
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
