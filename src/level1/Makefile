CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic
CPPFLAGS ?=
LDLIBS ?=

.PHONY: all test test-ndebug sanitize inspect-ci inspect-bench inspect-bench-ci evidence-ci evidence-bench evidence-bench-ci evidence-bench-sanitize-ci bench shadow-bench recorder-bench recorder-bench-ci psi-bench diagnostics-bench replay evaluation-ci evaluation-train evaluation-heldout evaluation-v05-ci evaluation-v05-development evaluation-v05-train evaluation-v05-heldout evaluation-v06-ci evaluation-v06-development evaluation-v06-train evaluation-v06-heldout evaluation-v07-ci evaluation-v07-development evaluation-v07-verification evaluation-v07-heldout clean
all: l1_monitor l1_trace_record l1_inspect l1_inspect_bench l1_evidence l1_evidence_bench l1_diagnostics l1_test l1_trace_test l1_state_test l1_shadow_test l1_record_test l1_linux_test l1_inspect_test l1_recorder_bench l1_psi_bench l1_bench l1_shadow_bench l1_replay l1_evaluate l1_evaluate_v05 l1_evaluate_v06

l1_monitor: l1_main.c l1_core.c l1_shadow.c l1_policy.c l1_state.c l1_linux.c l1.h l1_shadow.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_main.c l1_core.c l1_shadow.c l1_policy.c l1_state.c l1_linux.c $(LDLIBS)

l1_trace_record: l1_trace_record.c l1_trace.c l1_linux.c l1_trace.h l1_linux.h l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_trace_record.c l1_trace.c l1_linux.c $(LDLIBS)

l1_inspect: l1_inspect.c l1_demo.c l1_core.c l1_shadow.c l1_trace.c l1_demo.h l1_shadow.h l1_trace.h l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_inspect.c l1_demo.c l1_core.c l1_shadow.c l1_trace.c $(LDLIBS)

l1_inspect_test: l1_inspect_test.c l1_demo.c l1_core.c l1_shadow.c l1_demo.h l1_shadow.h l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_inspect_test.c l1_demo.c l1_core.c l1_shadow.c $(LDLIBS)

l1_inspect_bench: l1_inspect_bench.c l1_core.c l1_shadow.c l1_trace.c l1_shadow.h l1_trace.h l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_inspect_bench.c l1_core.c l1_shadow.c l1_trace.c $(LDLIBS)

l1_evidence: l1_evidence.c l1_core.c l1_shadow.c l1_trace.c l1_shadow.h l1_trace.h l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_evidence.c l1_core.c l1_shadow.c l1_trace.c $(LDLIBS)

l1_evidence_bench: l1_evidence_bench.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_evidence_bench.c $(LDLIBS)

l1_test: l1_test.c l1_core.c l1_policy.c l1_linux.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_test.c l1_core.c l1_policy.c l1_linux.c $(LDLIBS)

l1_trace_test: l1_trace_test.c l1_core.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_trace_test.c l1_core.c $(LDLIBS)

l1_state_test: l1_state_test.c l1_state.c l1_core.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_state_test.c l1_state.c l1_core.c $(LDLIBS)

l1_shadow_test: l1_shadow_test.c l1_shadow.c l1_core.c l1_shadow.h l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_shadow_test.c l1_shadow.c l1_core.c $(LDLIBS)

l1_record_test: l1_record_test.c l1_trace.c l1_trace.h l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_record_test.c l1_trace.c $(LDLIBS)

l1_linux_test: l1_linux_test.c l1_linux.c l1_linux.h l1.h
	$(CC) $(CPPFLAGS) -DL1_TESTING $(CFLAGS) -o $@ l1_linux_test.c l1_linux.c $(LDLIBS)

l1_recorder_bench: l1_recorder_bench.c l1_linux.c l1_trace.c l1_linux.h l1_trace.h l1.h
	$(CC) $(CPPFLAGS) -DL1_TESTING $(CFLAGS) -o $@ l1_recorder_bench.c l1_linux.c l1_trace.c $(LDLIBS)

l1_psi_bench: l1_psi_bench.c l1_linux.c l1_linux.h l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_psi_bench.c l1_linux.c $(LDLIBS)

l1_bench: l1_bench.c l1_core.c l1_linux.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_bench.c l1_core.c l1_linux.c $(LDLIBS)

l1_shadow_bench: l1_shadow_bench.c l1_core.c l1_shadow.c l1_linux.c l1.h l1_shadow.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_shadow_bench.c l1_core.c l1_shadow.c l1_linux.c $(LDLIBS)

l1_replay: l1_replay.c l1_core.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ l1_replay.c l1_core.c $(LDLIBS)

l1_evaluate: evaluation/l1_evaluate.c l1_core.c l1_policy.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ evaluation/l1_evaluate.c l1_core.c l1_policy.c $(LDLIBS)

l1_evaluate_v05: evaluation/l1_evaluate_v05.c l1_core.c l1_policy.c l1.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ evaluation/l1_evaluate_v05.c l1_core.c l1_policy.c $(LDLIBS)

l1_evaluate_v06: evaluation/l1_evaluate_v06.c l1_core.c l1_shadow.c l1_policy.c l1.h l1_shadow.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ evaluation/l1_evaluate_v06.c l1_core.c l1_shadow.c l1_policy.c $(LDLIBS) -lm

l1_diagnostics: evaluation/l1_diagnostics.c l1_core.c l1_shadow.c l1_trace.c l1.h l1_shadow.h l1_trace.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ evaluation/l1_diagnostics.c l1_core.c l1_shadow.c l1_trace.c $(LDLIBS) -lm

test: l1_test l1_trace_test l1_state_test l1_shadow_test l1_record_test l1_linux_test l1_inspect l1_inspect_test l1_evidence l1_evidence_bench l1_trace_record l1_diagnostics l1_recorder_bench l1_psi_bench l1_bench l1_shadow_bench l1_replay l1_evaluate l1_evaluate_v05 l1_evaluate_v06
	./l1_test
	./l1_trace_test
	./l1_state_test
	./l1_shadow_test
	./l1_record_test
	./l1_linux_test
	./l1_inspect_test
	sh ./inspect_args_test.sh
	sh ./evidence_args_test.sh
	sh ./evidence_bench_args_test.sh
	sh ./trace_args_test.sh
	sh ./recorder_bench_args_test.sh
	sh ./diagnostics_args_test.sh
	sh ./benchmark_args_test.sh
	sh ./evaluation_args_test.sh
	sh ./evaluation_v05_args_test.sh
	sh ./evaluation_v06_args_test.sh

test-ndebug:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(CFLAGS) -DNDEBUG' all test

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Werror -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' all test
	./l1_replay 100

inspect-ci: l1_inspect l1_inspect_test
	./l1_inspect_test
	sh ./inspect_args_test.sh
	./l1_inspect demo --details 0

inspect-bench: l1_inspect l1_inspect_bench
	sh ./run_inspect_benchmark.sh 86400 5

inspect-bench-ci: l1_inspect l1_inspect_bench
	sh ./run_inspect_benchmark.sh 1000 1

evidence-ci: l1_evidence
	sh ./evidence_args_test.sh

evidence-bench: l1_evidence l1_evidence_bench
	sh ./evidence_bench_args_test.sh
	sh ./run_evidence_benchmark.sh 86400 5

evidence-bench-ci: l1_evidence l1_evidence_bench
	sh ./evidence_bench_args_test.sh
	sh ./run_evidence_benchmark.sh 65 1

evidence-bench-sanitize-ci: l1_evidence l1_evidence_bench
	sh ./evidence_bench_args_test.sh
	sh ./run_evidence_benchmark.sh 65 1 instrumented

bench: l1_bench
	sh ./run_benchmarks.sh 5 10000 10000000

shadow-bench: l1_shadow_bench
	sh ./run_shadow_benchmarks.sh 5 10000 10000000

recorder-bench: l1_recorder_bench
	sh ./run_recorder_benchmarks.sh 5 full

recorder-bench-ci: l1_recorder_bench
	sh ./run_recorder_benchmarks.sh 1 ci

psi-bench: l1_psi_bench
	sh ./run_psi_benchmarks.sh 5

diagnostics-bench: l1_diagnostics
	./l1_diagnostics verification 5

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

EVAL_V06_MANIFEST ?= /tmp/l1-v06-training.eval.manifest
evaluation-v06-ci: l1_evaluate_v06
	sh ./evaluation/run_ci_v06.sh

evaluation-v06-development: l1_evaluate_v06
	./l1_evaluate_v06 development

evaluation-v06-train: l1_evaluate_v06
	./l1_evaluate_v06 train $(EVAL_V06_MANIFEST)

evaluation-v06-heldout: l1_evaluate_v06
	./l1_evaluate_v06 heldout $(EVAL_V06_MANIFEST) $(EVAL_REPETITIONS)

evaluation-v07-ci: l1_diagnostics
	sh ./evaluation/run_ci_v07.sh

evaluation-v07-development: l1_diagnostics
	./l1_diagnostics development 1

evaluation-v07-verification: l1_diagnostics
	./l1_diagnostics verification 1

evaluation-v07-heldout: l1_diagnostics
	./l1_diagnostics heldout $(EVAL_REPETITIONS)

clean:
	rm -f l1_monitor l1_trace_record l1_inspect l1_inspect_bench l1_evidence l1_evidence_bench l1_diagnostics l1_test l1_trace_test l1_state_test l1_shadow_test l1_record_test l1_linux_test l1_inspect_test l1_recorder_bench l1_psi_bench l1_bench l1_shadow_bench l1_replay l1_evaluate l1_evaluate_v05 l1_evaluate_v06
