$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'config.ps1')
$c = Get-LabConfig
Write-Host "HMI Lab directory: $($c.ProjectRoot)"
Write-Host "Virtual CPU: $($c.Cpus), RAM: $($c.RamMB) MB, disk: $($c.DiskSize)"
Write-Host "Acceleration: $(if ($c.UseWHPX) { 'WHPX' } else { 'TCG (software; slower)' })"
foreach ($tool in @('qemu-system-x86_64','qemu-img')) {
    try {
        $exe = Find-QemuTool $tool
        Write-Host "[OK] $tool : $exe"
        & $exe --version | Select-Object -First 1
    } catch { Write-Warning $_.Exception.Message }
}
foreach ($tool in @('ssh','scp')) {
    if (Get-Command $tool -ErrorAction SilentlyContinue) { Write-Host "[OK] $tool found" }
    else { Write-Warning "$tool missing. Install Windows OpenSSH Client optional feature." }
}
if (Test-Path -LiteralPath $c.IsoPath) { Write-Host "[OK] Debian installer ISO found" }
else { Write-Warning "Place Debian amd64 netinst ISO at $($c.IsoPath)" }
if (Test-Path -LiteralPath $c.DiskPath) { Write-Host "[OK] Virtual disk exists" }
else { Write-Host "[INFO] Virtual disk has not been created (expected on first run)." }
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    Write-Host "Host CPU: $($cpu.Name); firmware virtualization enabled: $($cpu.VirtualizationFirmwareEnabled)"
} catch { Write-Warning "Cannot read virtualization flag: $_" }
Write-Host 'Note: This checks tools and files, not whether the full Debian installation will succeed.'
