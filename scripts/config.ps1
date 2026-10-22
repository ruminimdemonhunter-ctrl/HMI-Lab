# Host-side configuration. Windows source of truth; guest source is disposable.
function Get-LabConfig {
    $root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    # WHPX requires enabling an optional Windows feature. Stay with TCG by default.
    $UseWHPX = $true
    return @{
        ProjectRoot = $root
        VmDir = Join-Path $root 'vm'
        DiskPath = Join-Path $root 'vm\disks\debian-lab.qcow2'
        IsoPath = Join-Path $root 'vm\iso\debian.iso'
        SourceDir = Join-Path $root 'src'
        BackupDir = Join-Path $root 'vm\backups'
        GuestUser = 'lab'
        SshPort = 2222
        RamMB = 2048
        Cpus = 2
        DiskSize = '16G'
        UseWHPX = $UseWHPX
    }
}
function Find-QemuTool {
    param([Parameter(Mandatory=$true)][string]$Name)
    $found = Get-Command "$Name.exe" -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    $default = Join-Path ${env:ProgramFiles} ("qemu\$Name.exe")
    if (Test-Path -LiteralPath $default) { return $default }
    throw "Could not find $Name.exe. Install QEMU for Windows and ensure it is in PATH or C:\Program Files\qemu."
}
