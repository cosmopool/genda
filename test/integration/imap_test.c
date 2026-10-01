// imap module tests: without GENDA_IMAP_URL the thread is a no-op.
// The live poll path needs real credentials — exercised manually.
#include "../../src/imap.h"
#include "../vendor/greatest.h"

#include <stdlib.h>

TEST threadNoOpsWithoutUrl(void) {
  unsetenv("GENDA_IMAP_URL");
  ASSERT(imapThread(NULL) == NULL);
  PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
  GREATEST_MAIN_BEGIN();
  RUN_TEST(threadNoOpsWithoutUrl);
  GREATEST_MAIN_END();
  (void)argc;
  (void)argv;
}
