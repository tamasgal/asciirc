CC ?= cc

CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra
CPPFLAGS += -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE

SRC = src/json.c src/util.c
OBJ = $(SRC:.c=.o)
HDR = src/json.h src/util.h

all: $(OBJ)

$(OBJ): $(HDR)

clean:
	rm -f $(OBJ)

.PHONY: all clean
