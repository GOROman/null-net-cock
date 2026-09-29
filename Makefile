CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
UNAME_S := $(shell uname -s)
LDLIBS = -lpthread -lsqlite3
ifeq ($(UNAME_S),Darwin)
LDLIBS += -liconv
endif
SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)

null-net-cock: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c src/nc.h src/db.h src/msg.h src/session.h src/xfer.h src/help.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/filem_test: tests/filem_test.c src/db.c src/util.c src/db.h src/nc.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/filem_test.c src/db.c src/util.c $(LDLIBS)

test: null-net-cock build/filem_test
	./build/filem_test
	python3 tests/e2e.py ./null-net-cock
	python3 tests/xfer.py ./null-net-cock
	python3 tests/modem.py ./null-net-cock
	python3 tests/sysop.py ./null-net-cock
	python3 tests/member.py ./null-net-cock
	python3 tests/host.py ./null-net-cock
	python3 tests/console.py ./null-net-cock
	python3 tests/help.py ./null-net-cock
	python3 tests/ws.py ./null-net-cock
	python3 tests/signup.py ./null-net-cock

clean:
	rm -rf build null-net-cock

.PHONY: test clean
