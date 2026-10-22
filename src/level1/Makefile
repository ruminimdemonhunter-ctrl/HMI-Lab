CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic
CPPFLAGS ?=
LDLIBS ?=

.PHONY: all test test-ndebug sanitize bench replay evaluation-ci evaluation-train evaluation-heldout evaluation-v05-ci evaluation-v05-development evaluation-v05-train evaluation-v05-heldout clean
all: l1_monitor l1_test l1_trace_test l1_state_test l1_bench l1_replay l1_evaluate l1_evaluate_v05

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

l1_evaluate: evaluation/l1_evaluate.c l1_core.c l1_policy.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ evaluation/l1_evaluate.c l1_core.c l1_policy.c $(LDLIBS)

l1_evaluate_v05: evaluation/l1_evaluate_v05.c l1_core.c l1_policy.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ evaluation/l1_evaluate_v05.c l1_core.c l1_policy.c $(LDLIBS)

test: l1_test l1_trace_test l1_state_test l1_bench l1_evaluate l1_evaluate_v05
	./l1_test
	./l1_trace_test
	./l1_state_test
	sh ./benchmark_args_test.sh
	sh ./evaluation_args_test.sh
	sh ./evaluation_v05_args_test.sh

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

evaluation-ci: l1_evaluate
	sh ./evaluation/run_ci.sh

EVAL_POLICY ?= /tmp/l1-v04-tuned.eval.policy
EVAL_REPETITIONS ?= 100
evaluation-train: l1_evaluate
	./l1_evaluate train $(EVAL_POLICY)

evaluation-heldout: l1_evaluate
	./l1_evaluate heldout $(EVAL_POLICY) $(EVAL_REPETITIONS)

EVAL_V05_MANIFEST ?= /tmp/l1-v05-training.eval.manifest
evaluation-v05-ci: l1_evaluate_v05
	sh ./evaluation/run_ci_v05.sh

evaluation-v05-development: l1_evaluate_v05
	./l1_evaluate_v05 development

evaluation-v05-train: l1_evaluate_v05
	./l1_evaluate_v05 train $(EVAL_V05_MANIFEST)

evaluation-v05-heldout: l1_evaluate_v05
	./l1_evaluate_v05 heldout $(EVAL_V05_MANIFEST) $(EVAL_REPETITIONS)

clean:
	rm -f l1_monitor l1_test l1_trace_test l1_state_test l1_bench l1_replay l1_evaluate l1_evaluate_v05
