$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'config.ps1')
$c = Get-LabConfig
if (-not (Test-Path -LiteralPath $c.DiskPath)) { throw 'No VM disk exists yet.' }
Write-Host 'IMPORTANT: Shut down Debian with sudo poweroff, and make sure QEMU is closed.'
$answer = Read-Host 'Type SHUTDOWN to confirm the VM is completely stopped'
if ($answer -cne 'SHUTDOWN') { throw 'Cancelled: never back up a disk while QEMU is still writing to it.' }
New-Item -ItemType Directory -Force -Path $c.BackupDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dst = Join-Path $c.BackupDir "debian-lab-$stamp.qcow2"
Copy-Item -LiteralPath $c.DiskPath -Destination $dst -ErrorAction Stop
Write-Host "Backup created: $dst"
