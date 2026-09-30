CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE
XCRUN_SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
ifneq ($(XCRUN_SDK),)
CFLAGS += -isysroot $(XCRUN_SDK)
endif
LDLIBS = -lsqlite3 -lcurl -lpthread

all: genda

genda: genda.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: genda test_integration
	./test_integration

test_integration: test_integration.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f genda test_integration *.o

.PHONY: all test clean
