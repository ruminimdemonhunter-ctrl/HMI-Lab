CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic
CPPFLAGS ?=
LDLIBS ?=

.PHONY: all test test-ndebug sanitize bench replay clean
all: l1_monitor l1_test l1_trace_test l1_state_test l1_bench l1_replay

l1_monitor: l1_main.c l1_core.c l1_policy.c l1_state.c l1_linux.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_main.c l1_core.c l1_policy.c l1_state.c l1_linux.c $(LDLIBS)

l1_test: l1_test.c l1_core.c l1_policy.c l1_linux.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_test.c l1_core.c l1_policy.c l1_linux.c $(LDLIBS)

l1_trace_test: l1_trace_test.c l1_core.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_trace_test.c l1_core.c $(LDLIBS)

l1_state_test: l1_state_test.c l1_state.c l1_core.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_state_test.c l1_state.c l1_core.c $(LDLIBS)

l1_bench: l1_bench.c l1_core.c l1_linux.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_bench.c l1_core.c l1_linux.c $(LDLIBS)

l1_replay: l1_replay.c l1_core.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_replay.c l1_core.c $(LDLIBS)

test: l1_test l1_trace_test l1_state_test l1_bench
	./l1_test
	./l1_trace_test
	./l1_state_test
	sh ./benchmark_args_test.sh

test-ndebug:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(CFLAGS) -DNDEBUG' all test

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' all test
	./l1_replay 100

bench: l1_bench
	sh ./run_benchmarks.sh 5 10000 10000000

replay: l1_replay
	sh ./run_replays.sh 5 10000

clean:
	rm -f l1_monitor l1_test l1_trace_test l1_state_test l1_bench l1_replay
