# L1 design agenda — next discussion

Before implementation answer:

1. What is "fundamental"? CPU scheduling, memory pressure, energy, or a virtual device controller? Choose **one** for a first benchmark.
2. What sensors are readable in the Debian VM? `/proc/stat`, `/proc/meminfo`, PSI `/proc/pressure/*`, per-process status, wall time.
3. What actuators are allowed and safe? Initially prefer non-privileged simulated knobs; graduate to cgroup-based controls only in the guest with an explicit permission boundary.
4. What is the decision interval? Keep it inexpensive and bounded; no LLM on the hot scheduling path.
5. What is the fallback controller if L1 crashes or performs worse?
6. How do we measure power or thermal behavior when QEMU does not expose real physical sensors? Use simulated signals and external host observations, not claims of real thermal control.
7. What is success? e.g., no worse than baseline latency, no starvation, measurable improvement in a selected workload, controller overhead within a predefined budget.

The software foundation only validates isolation, boot, SSH, source deployment, and baseline telemetry. It does NOT prove hardware-level AI performance.
