# userrun.ps1 -Exe path [-Args "..."] [-Wait 60] - run a program in the logged-on user's session (runuser2.ps1) with
# stdout/stderr captured to C:\S10eTmp\userrun.out, then print that output. For tools that need the interactive desktop
# (display DCs, AppContainers) when driven over SSH, where quoting through runuser2's -Cmd breaks.
param([Parameter(Mandatory)][string]$Exe, [string]$Args = '', [int]$Wait = 60)
$out = 'C:\S10eTmp\userrun.out'
Remove-Item $out -ErrorAction SilentlyContinue
& C:\S10eTmp\runuser2.ps1 -Cmd "`"$Exe`" $Args > $out 2>&1" -Wait $Wait | Out-Null
Get-Content $out -ErrorAction SilentlyContinue
