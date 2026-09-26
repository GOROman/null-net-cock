CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDLIBS = -lpthread -liconv -lsqlite3
SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

null-net-cock: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c src/nc.h src/db.h src/msg.h src/session.h src/xfer.h src/help.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

test: null-net-cock
	python3 tests/e2e.py ./null-net-cock
	python3 tests/xfer.py ./null-net-cock
	python3 tests/modem.py ./null-net-cock
	python3 tests/sysop.py ./null-net-cock
	python3 tests/member.py ./null-net-cock
	python3 tests/host.py ./null-net-cock
	python3 tests/console.py ./null-net-cock
	python3 tests/help.py ./null-net-cock
	python3 tests/ws.py ./null-net-cock

clean:
	rm -rf build null-net-cock

.PHONY: test clean
