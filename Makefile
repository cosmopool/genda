CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE
XCRUN_SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
ifneq ($(XCRUN_SDK),)
CFLAGS += -isysroot $(XCRUN_SDK)
endif
LDLIBS = -lsqlite3 -lcurl -lpthread

SRCS = src/main.c src/server.c src/msgpack.c src/db.c src/classify.c src/imap.c
OBJS = $(SRCS:.c=.o)
HDRS = src/common.h src/server.h src/msgpack.h src/db.h src/classify.h src/imap.h

all: genda

genda: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): $(HDRS)

test: genda test_integration
	./test_integration

test_integration: test/test_integration.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f genda test_integration src/*.o

.PHONY: all test clean
