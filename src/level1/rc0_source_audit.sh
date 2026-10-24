#!/bin/sh
set -eu

sha256sum -c rc0_source_manifest.sha256

if grep -En '/proc|/sys|cgroup|sched_|socket[[:space:]]*\(|ioctl[[:space:]]*\(|system[[:space:]]*\(|fork[[:space:]]*\(|exec[[:alnum:]_]*[[:space:]]*\(|open[[:space:]]*\(|fopen[[:space:]]*\(|write[[:space:]]*\(' l1_core.c l1_shadow.c >/dev/null; then
    echo "deterministic core or shadow contains a forbidden external interface" >&2
    exit 1
fi

if grep -En 'cgroup|sched_|/sys/|/dev/|ioctl[[:space:]]*\(|setpriority[[:space:]]*\(' l1_inspect.c l1_evidence.c l1_proposal_check.c >/dev/null; then
    echo "offline inspection boundary contains a forbidden control interface" >&2
    exit 1
fi

if nm l1_proposal_check | grep -q 'l1_proposal_test_apply_clone'; then
    echo "production offline proposal checker exposes the test-only apply seam" >&2
    exit 1
fi

echo "PASS: protected-source identities and read-only/test-only linkage audit"
