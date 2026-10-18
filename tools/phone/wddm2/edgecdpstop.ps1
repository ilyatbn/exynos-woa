# (user session, via run-as-user.ps1, which passes no arguments) close the DevTools Edge instance of edgecdp.ps1.
& (Join-Path $PSScriptRoot 'edgecdp.ps1') -Stop
