# HMI Lab design boundaries

- **Windows host**: VS Code, Codex, Git, QEMU binary, source of truth, offline VM backups.
- **Debian guest**: isolated experimental testbed with SSH server and deployed source copy.
- **L1 future research**: local low-cost control law/model; emulated hardware and Linux telemetry only. Separate a safety guard from an adaptive policy.
- **L2 future research**: explicit OS resource policy orchestration, not an LLM in the kernel.
- **L3 future research**: occasional strategic reasoning (on-device or cloud), advisory only.
- **Core rule**: high-level AI suggests goals; lower-level policies enforce limits. Conventional OS and hardware safety rules always win.
- **Trust boundary**: no raw physical disk passthrough, host filesystem mount, privileged host device control, or bridged network. QEMU guest internet is permitted only for installation/explicit package-setup sessions; day-to-day VM start uses QEMU `restrict=on`, with the explicit Windows loopback SSH forwarding still allowed.
- **Data flow**: `Windows src/` --(SCP over host loopback port 2222)--> `/home/lab/hmi/src/`. Sync destroys and replaces the guest source tree, but preserves guest OS and data outside this tree. VM changes are not synced back.
- **Testing**: compare any L1 candidate to built-in Linux policies with overhead included; evaluate throughput, interactivity, memory pressure, and control stability.
