# (user session, via runapp.sh) storestate.ps1 - is the Microsoft Store (and its install service) present for this
# user? -> C:\S10eTmp\storestate.out (winget msstore installs failed 0x80004002; notes/games-steam.md).
$o = 'C:\S10eTmp\storestate.out'
Get-AppxPackage -Name Microsoft.WindowsStore, Microsoft.StorePurchaseApp, Microsoft.DesktopAppInstaller, Microsoft.Services.Store.Engagement -ErrorAction SilentlyContinue |
  ForEach-Object { "{0} {1} {2}" -f $_.Name, $_.Version, $_.Architecture } | Set-Content $o
Get-Service InstallService, ClipSVC, AppXSvc, wuauserv -ErrorAction SilentlyContinue | ForEach-Object { "svc {0} {1} {2}" -f $_.Name, $_.Status, $_.StartType } | Add-Content $o
