$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'config.ps1')
$c = Get-LabConfig
& ssh -p ([string]$c.SshPort) "$($c.GuestUser)@127.0.0.1"
