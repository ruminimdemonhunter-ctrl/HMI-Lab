$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'config.ps1')
$c = Get-LabConfig
foreach ($tool in @('ssh','scp')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool missing (Windows OpenSSH Client)." }
}
if (-not (Test-Path -LiteralPath $c.SourceDir)) { throw "Source folder missing: $($c.SourceDir)" }
$user = $c.GuestUser
$port = [string]$c.SshPort
$target = "${user}@127.0.0.1"
$remote = "/home/$user/hmi"
Write-Host "Deploying $($c.SourceDir) to ${target}:$remote/src (one-way, guest copy replaced)."
# Deliberately scoped to the guest user's /home/.../hmi tree; never touches Windows files.
& ssh -o StrictHostKeyChecking=accept-new -p $port $target "mkdir -p '$remote/.staging' && rm -rf '$remote/.staging/src'"
if ($LASTEXITCODE -ne 0) { throw 'SSH preparation failed. Is Debian running with openssh-server installed?' }
& scp -r -P $port $c.SourceDir "${target}:$remote/.staging/"
if ($LASTEXITCODE -ne 0) { throw 'SCP transfer failed; existing guest source has not been replaced.' }
& ssh -p $port $target "test -d '$remote/.staging/src' && rm -rf '$remote/src' && mv '$remote/.staging/src' '$remote/src'"
if ($LASTEXITCODE -ne 0) { throw 'Guest source replacement failed.' }
Write-Host 'Deployment successful. On guest: python3 /home/lab/hmi/src/smoke_test.py'
