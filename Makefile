CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE
XCRUN_SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
ifneq ($(XCRUN_SDK),)
CFLAGS += -isysroot $(XCRUN_SDK)
endif
LDLIBS = -lsqlite3 -lcurl -lpthread

SRCS = main.c server.c msgpack.c db.c classify.c imap.c
OBJS = $(SRCS:.c=.o)
HDRS = common.h server.h msgpack.h db.h classify.h imap.h

all: genda

genda: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): $(HDRS)

test: genda test_integration
	./test_integration

test_integration: test_integration.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f genda test_integration *.o

.PHONY: all test clean
