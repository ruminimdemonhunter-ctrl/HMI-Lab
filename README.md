# HMI Lab — Windows + QEMU Development Foundation

This workspace is a **safe-ish VM lab**, not a host OS modification project. The host is Windows; the guest is minimal Debian Linux. Source code lives only in Windows and is sent one way into the VM via loopback SSH.

**Status: Level 1 v0.3 research prototype.** A native C11, read-only pressure
observer and bounded recommendation engine now lives in `src/level1`. It has no
actuator and makes no resource-control changes. V0.3 adds monotonic adaptation
gates, corruption-detecting local behavior-memory persistence, deterministic
replay experiments, and CI. See `docs/L1-v0.3-validation.md` for results and
limitations.

## Prerequisites

- 64-bit Intel/AMD Windows 10/11 PC, ideally >=8 GB physical RAM and >=25 GB free disk space.
- QEMU x86_64 Windows installer: https://www.qemu.org/download/ (follow 64-bit Windows link).
- Debian stable **amd64 netinst** ISO: https://www.debian.org/CD/netinst/
- VS Code; Codex on Windows; Windows built-in `ssh` and `scp` commands (OpenSSH Client).
- Optional: Windows Hypervisor Platform (WHPX) for speed. It changes an optional Windows feature and may require a reboot; the lab defaults to software emulation (`tcg`) until you opt in.

## Windows installation and first run (PowerShell)

1. Extract the whole `HMI-Lab` folder to a short local path, e.g. `C:\Dev\HMI-Lab`. Do **not** extract directly under Program Files or a cloud-synced folder.
2. Install QEMU from the official download page. Default path assumed: `C:\Program Files\qemu`.
3. Download the Debian **amd64 netinst** ISO, rename it `debian.iso`, and place it in `HMI-Lab\vm\iso\debian.iso`.
4. In a PowerShell terminal from `HMI-Lab`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-prereqs.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\create-disk.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-install.ps1
```

5. In Debian installer choose text/graphical install, automatic DHCP, host name `hmi-vm`, user name **`lab`** (must match `scripts/config.ps1`). Leave *root password blank* so Debian configures `sudo` for your first user. When guided partitioning appears, check that the selected target is the VM's **16 GB virtual disk**, not a physical Windows disk. Choose whole disk guided install **inside the VM**. Software selection: uncheck desktop environment, check **SSH server** and **standard system utilities**. GRUB should install on the virtual disk. After installation finishes and Debian restarts, shut down Linux gracefully.
6. For the **one-time Debian package setup**, start the installed VM with network access:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-vm.ps1 -AllowGuestInternet
```

Once dependencies are installed, shut Debian down and later launch the normal VM with `start-vm.ps1` **without** `-AllowGuestInternet`: outbound guest network traffic will be blocked while loopback SSH forwarding keeps working.

7. Sign in to Debian console and install tools:

```bash
sudo apt update
sudo apt install -y openssh-server python3 git procps build-essential
sudo systemctl enable --now ssh
systemctl is-active ssh
```

8. While QEMU is running, open a **second Windows PowerShell window** and run:

```powershell
ssh -p 2222 lab@127.0.0.1
```

9. One-way deploy of Windows `src/` folder into `/home/lab/hmi/src/`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\sync-source.ps1
```

10. Confirm code in VM:

```powershell
ssh -p 2222 lab@127.0.0.1 "python3 /home/lab/hmi/src/smoke_test.py"
```

## VS Code

Open the `HMI-Lab` folder. Use **Terminal → Run Task**:

- HMI: Check prerequisites
- HMI: Create disk (first run only)
- HMI: Install Debian (first run only)
- HMI: Start VM (isolated)
- HMI: Start VM with internet (package setup)
- HMI: Deploy source to VM
- HMI: SSH into VM
- HMI: Backup VM (after shutdown)

Keep QEMU running in its own terminal/window. Editing `src/` with Codex or VS Code changes the Windows source. The deploy task transfers a fresh copy to Linux; this is **on-demand sync, not a live-mounted folder**. The VM's `/home/lab/hmi/src` is replaced on each deploy, and any changes made there will be lost. Keep editable source on Windows.

## Backups

Shut the guest down using `sudo poweroff`, close its QEMU process, then run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\backup-vm.ps1
```

Backups are placed in `vm/backups/` and **not** in Git. QEMU's disk-image tooling warns not to modify an image while a VM is using it.

## Optional WHPX (speed-up)

- Only if you are comfortable enabling a Windows virtualization feature: run `optionalfeatures.exe` and check **Windows Hypervisor Platform** (not necessarily full Hyper-V), restart if requested.
- Edit `scripts/config.ps1` and change `$UseWHPX = $false` to `$true`.
- Keep software-emulation (`tcg`) if you prefer no optional Windows-feature changes, but Debian installation and workloads can be much slower.

## Safe-work rules

- Never run host commands that partition disks, attach real physical disks, or configure PCI/USB passthrough.
- Keep all guest disk images inside `vm/`; the scripts use only that QCOW2 file.
- Guest SSH is forwarded only to `127.0.0.1:2222` on Windows. Normal VM starts with QEMU `restrict=on`: outbound guest traffic cannot access the wider network or host, while the explicit SSH forwarding remains functional. Installer/optional package-setup sessions use outbound NAT. No bridged networking is configured.
- Host -> guest deploy is deliberately one way; do not mount your Windows system partition in the guest.
- Guest shell commands are NOT Windows PowerShell commands.
- QEMU is security-sensitive software; use trusted installers, OS images, and updates. Virtualization greatly reduces accidental host-OS risk but does not guarantee perfect containment.
- Don't start two QEMU processes with the same writable disk image.

## Current research phase

Validate Level 1 decisions and observer overhead before discussing any
actuator. No automatic scheduling changes are present or authorized.

See `docs/architecture.md`, `docs/L1-v0.3-validation.md`, and
`docs/level1-questions.md`.
