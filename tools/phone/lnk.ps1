# lnk.ps1 [-Match Kairo] - resolve desktop shortcuts (user + public desktop): target, arguments, working directory.
param([string]$Match = '')
$sh = New-Object -ComObject WScript.Shell
Get-ChildItem 'C:\Users\*\Desktop\*.lnk', 'C:\Users\Public\Desktop\*.lnk' -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match $Match } | ForEach-Object {
    $s = $sh.CreateShortcut($_.FullName)
    '{0}: "{1}" {2} (in {3})' -f $_.Name, $s.TargetPath, $s.Arguments, $s.WorkingDirectory
  }
