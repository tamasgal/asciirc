CC ?= cc

CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra
CPPFLAGS += -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -I/opt/homebrew/opt/openssl/include
LDLIBS += -L/opt/homebrew/opt/openssl/lib -lssl -lcrypto

SRC = src/net.c src/json.c src/util.c
OBJ = $(SRC:.c=.o)
HDR = src/net.h src/json.h src/util.h

all: $(OBJ)

$(OBJ): $(HDR)

clean:
	rm -f $(OBJ)

.PHONY: all clean
