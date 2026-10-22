param([switch]$AllowGuestInternet)
& (Join-Path $PSScriptRoot 'launch.ps1') -AllowGuestInternet:$AllowGuestInternet
