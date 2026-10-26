CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic
CPPFLAGS ?=
LDLIBS ?=
FIXTURES = evaluation/fixtures_v1

.PHONY: all test test-ndebug sanitize corpus-verify bench audit ci qualification clean
all: l2_fixture_cli l2_test l2_l1_isolation_test l2_bench

l2_fixture_cli: l2_fixture_cli.c l2_parser.c l2_state.c l2_parser.h l2_state.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l2_fixture_cli.c l2_parser.c l2_state.c $(LDLIBS)

l2_test: l2_test.c l2_parser.c l2_state.c l2_parser.h l2_state.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l2_test.c l2_parser.c l2_state.c $(LDLIBS)

l2_l1_isolation_test: l2_l1_isolation_test.c l2_state.c l2_state.h ../level1/l1_core.c ../level1/l1_shadow.c ../level1/l1.h ../level1/l1_shadow.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I../level1 -o $@ l2_l1_isolation_test.c l2_state.c ../level1/l1_core.c ../level1/l1_shadow.c $(LDLIBS)

l2_bench: l2_bench.c l2_parser.c l2_state.c l2_parser.h l2_state.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l2_bench.c l2_parser.c l2_state.c $(LDLIBS)

corpus-verify:
	python3 evaluation/verify_l2_v01_fixtures.py

test: all corpus-verify
	./l2_test $(FIXTURES)
	./l2_l1_isolation_test
	sh ./fixture_cli_test.sh
	sh ./benchmark_args_test.sh
	$(MAKE) audit

test-ndebug:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(CFLAGS) -DNDEBUG' all test

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' all test

bench: l2_bench
	./l2_bench 5 100000

audit:
	sh ./l2_source_audit.sh

ci:
	$(MAKE) clean all test
	$(MAKE) test-ndebug
	$(MAKE) clean all test
	$(MAKE) bench

qualification:
	$(MAKE) ci
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(MAKE) sanitize
	$(MAKE) clean all test bench
	$(MAKE) -C ../level1 rc0-qualification

clean:
	rm -f l2_fixture_cli l2_test l2_l1_isolation_test l2_bench
