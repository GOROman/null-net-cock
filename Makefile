CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDLIBS = -lpthread -liconv
SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

null-net-cock: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c src/nc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

test: null-net-cock
	python3 tests/e2e.py ./null-net-cock

clean:
	rm -rf build null-net-cock

.PHONY: test clean
