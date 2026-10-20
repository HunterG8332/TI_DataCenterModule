# Host-side tests for the portable BMC logic (components/bmc_core).
# No ESP-IDF needed: build and run on a PC (Linux/WSL/macOS):
#     make -C firmware/BMC_firm/test_host test
CORE    := ../components/bmc_core
CC      ?= gcc
CFLAGS  := -std=c11 -D_GNU_SOURCE -g -O1 -Wall -Wextra -Wshadow -Werror \
           -fsanitize=address,undefined -fno-sanitize-recover=undefined -I$(CORE)/include -I.
SRCS    := $(wildcard $(CORE)/*.c)
TESTS   := $(wildcard test_*.c)

run_tests: $(SRCS) $(TESTS) test_util.h
	$(CC) $(CFLAGS) $(SRCS) $(TESTS) -o $@ -lm

test: run_tests
	./run_tests

lib-check:
	$(CC) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wshadow -Werror -I$(CORE)/include -c $(SRCS)
	@rm -f *.o

clean:
	rm -f run_tests *.o

.PHONY: test clean lib-check
