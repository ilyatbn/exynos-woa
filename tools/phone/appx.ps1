# appx.ps1 [-Match regex] - installed packaged apps (all users) whose name matches, with version and install path.
param([string]$Match = 'Zune|Photos|Media|Clipchamp|Video')
Get-AppxPackage -AllUsers | Where-Object { $_.Name -match $Match } |
  Select-Object Name, Version, Architecture, InstallLocation | Format-Table -AutoSize | Out-String -Width 220
