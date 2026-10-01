CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE
XCRUN_SDK := $(shell xcrun --show-sdk-path 2>/dev/null)
ifneq ($(XCRUN_SDK),)
CFLAGS += -isysroot $(XCRUN_SDK)
endif
LDLIBS = -lsqlite3 -lcurl -lpthread

SRCS = src/main.c src/server.c src/msgpack.c src/db.c src/classify.c src/imap.c src/config.c
OBJS = $(SRCS:.c=.o)
HDRS = src/core.h src/common.h src/config.h src/server.h src/msgpack.h src/db.h src/classify.h src/imap.h

all: genda

genda: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): $(HDRS)

test: genda msgpack_test db_test classify_test imap_test server_test
	./test/integration/msgpack_test && ./test/integration/db_test && \
	./test/integration/classify_test && ./test/integration/imap_test && \
	./test/integration/server_test

msgpack_test: test/integration/msgpack_test.c src/msgpack.o
	$(CC) $(CFLAGS) -o test/integration/msgpack_test test/integration/msgpack_test.c src/msgpack.o

db_test: test/integration/db_test.c src/db.o src/classify.o src/msgpack.o src/config.o
	$(CC) $(CFLAGS) -o test/integration/db_test test/integration/db_test.c src/db.o src/classify.o src/msgpack.o src/config.o $(LDLIBS)

classify_test: test/integration/classify_test.c src/classify.o
	$(CC) $(CFLAGS) -o test/integration/classify_test test/integration/classify_test.c src/classify.o

imap_test: test/integration/imap_test.c src/imap.o src/db.o src/classify.o src/msgpack.o src/config.o
	$(CC) $(CFLAGS) -o test/integration/imap_test test/integration/imap_test.c src/imap.o src/db.o src/classify.o src/msgpack.o src/config.o $(LDLIBS)

server_test: test/integration/server_test.c
	$(CC) $(CFLAGS) -DGENDA_BIN=\"$(CURDIR)/genda\" -o test/integration/server_test test/integration/server_test.c $(LDLIBS)

clean:
	rm -f genda test/integration/*_test src/*.o

.PHONY: all test clean
