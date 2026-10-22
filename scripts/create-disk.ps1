$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'config.ps1')
$c = Get-LabConfig
if (Test-Path -LiteralPath $c.DiskPath) {
    Write-Host "Disk already exists. Refusing to overwrite: $($c.DiskPath)"
    exit 0
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $c.DiskPath) | Out-Null
$img = Find-QemuTool 'qemu-img'
& $img create -f qcow2 $c.DiskPath $c.DiskSize
if ($LASTEXITCODE -ne 0) { throw "qemu-img create failed with exit code $LASTEXITCODE" }
Write-Host "Created VM-only disk: $($c.DiskPath)"
