param([switch]$Install, [switch]$AllowGuestInternet)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'config.ps1')
$c = Get-LabConfig
if (-not (Test-Path -LiteralPath $c.DiskPath)) { throw 'Disk missing. First run scripts/create-disk.ps1' }
if ($Install -and -not (Test-Path -LiteralPath $c.IsoPath)) { throw "Installer ISO missing: $($c.IsoPath)" }
$qemu = Find-QemuTool 'qemu-system-x86_64'
$accel = if ($c.UseWHPX) { 'whpx' } else { 'tcg' }
# Internet only when explicitly requested or during network installation.
$restrict = if ($Install -or $AllowGuestInternet) { 'off' } else { 'on' }
$args = @(
    '-name', 'HMI-Lab',
    '-machine', 'q35',
    '-accel', $accel,
    '-m', [string]$c.RamMB,
    '-smp', [string]$c.Cpus,
    '-drive', "file=$($c.DiskPath),if=virtio,format=qcow2",
    '-nic', "user,model=virtio-net-pci,restrict=$restrict,hostfwd=tcp:127.0.0.1:$($c.SshPort)-:22",
    '-vga', 'std'
)
if ($Install) {
    $args += @('-cdrom', $c.IsoPath, '-boot', 'once=d')
}
Write-Host "Starting VM using $accel. Guest internet enabled: $($restrict -eq 'off'). Close only after shutting down the guest (sudo poweroff)."
& $qemu @args
if ($LASTEXITCODE -ne 0) { throw "QEMU exited with error code $LASTEXITCODE. If WHPX fails, see README for TCG fallback." }
