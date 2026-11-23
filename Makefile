PREFIX ?= /usr/local
CC ?= cc
PKG_CONFIG ?= pkg-config

OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || echo -lssl -lcrypto)

CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -pedantic
CPPFLAGS += -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE $(OPENSSL_CFLAGS)
LDLIBS += $(OPENSSL_LIBS)

SRC = src/main.c src/app.c src/config.c src/ui.c src/rc.c src/ws.c src/http.c src/net.c src/json.c src/util.c
OBJ = $(SRC:.c=.o)
HDR = src/app.h src/config.h src/ui.h src/rc.h src/ws.h src/http.h src/net.h src/json.h src/util.h

TEST_OBJ = tests/test.o src/config.o src/json.o src/util.o src/net.o src/http.o src/ws.o src/ui.o

all: asciirc

asciirc: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ) tests/test.o: $(HDR)

tests/test: $(TEST_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJ) $(LDLIBS)

check: tests/test
	./tests/test

install: asciirc
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp asciirc $(DESTDIR)$(PREFIX)/bin/asciirc

clean:
	rm -f asciirc $(OBJ) tests/test tests/test.o

.PHONY: all check install clean
