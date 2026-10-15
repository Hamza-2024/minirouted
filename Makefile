CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -D_GNU_SOURCE
SRC      = src/router.c src/proto.c src/spf.c src/rib.c src/netlink.c

all: minirouted tests/nltool

minirouted: $(SRC) src/minirouted.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

tests/nltool: tests/nltool.c src/minirouted.h
	$(CC) $(CFLAGS) -o $@ tests/nltool.c

# Simulated network on 127.0.0.1, no root needed.
test: all
	python3 tests/test_convergence.py

# Same tests, plus checks the routes really land in the Linux kernel.
# Runs inside a fresh network namespace so it can't touch the host's routes.
test-kernel: all
	sudo unshare -n python3 tests/test_convergence.py --kernel

# Same tests with AddressSanitizer + UBSan to catch memory bugs.
test-asan: clean
	$(MAKE) CFLAGS="$(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" all
	python3 tests/test_convergence.py

clean:
	rm -f minirouted tests/nltool

.PHONY: all test test-kernel test-asan clean
