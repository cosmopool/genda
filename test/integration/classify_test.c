// classify module smoke test. The heuristic is a placeholder (classify.h
// TODO: real LLM classifier), so only the contract is pinned, not its rules.
#include "../../src/classify.h"
#include "../../src/common.h"
#include "../vendor/greatest.h"

TEST smoke(void) {
  Input appt = {.title = "Dentist appointment", .text = "see you 2026-10-01T10:00"};
  Classified c = classifyInput(&appt);
  ASSERT(c.kind == KIND_NONE || c.kind == KIND_APPOINTMENT || c.kind == KIND_OBLIGATION);
  Input noise = {.title = "meme of the day", .text = "haha look at this"};
  ASSERT_EQ(0, classifyInput(&noise).is_event);
  PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_TEST(smoke);
  GREATEST_MAIN_END();
  (void)argc;
  (void)argv;
}
