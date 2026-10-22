CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic
CPPFLAGS ?=
LDLIBS ?=

.PHONY: all test bench clean
all: l1_monitor l1_test l1_trace_test l1_bench

l1_monitor: l1_main.c l1_core.c l1_policy.c l1_linux.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_main.c l1_core.c l1_policy.c l1_linux.c $(LDLIBS)

l1_test: l1_test.c l1_core.c l1_policy.c l1_linux.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_test.c l1_core.c l1_policy.c l1_linux.c $(LDLIBS)

l1_trace_test: l1_trace_test.c l1_core.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_trace_test.c l1_core.c $(LDLIBS)

l1_bench: l1_bench.c l1_core.c l1_linux.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_bench.c l1_core.c l1_linux.c $(LDLIBS)

test: l1_test l1_trace_test
	./l1_test
	./l1_trace_test

bench: l1_bench
	sh ./run_benchmarks.sh 5 10000 10000000

clean:
	rm -f l1_monitor l1_test l1_trace_test l1_bench
