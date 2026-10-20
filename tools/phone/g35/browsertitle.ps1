# browsertitle.ps1 [-Name msedge] - (user session, via runuser2.ps1) the browser windows' titles -> C:\S10eTmp\title.txt.
# play.html puts its frame counter in the title ("g35 <frames>"), so two reads show whether the video still plays.
param([string]$Name = 'msedge')
Get-Process $Name -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle } |
  ForEach-Object { "$(Get-Date -Format HH:mm:ss) $($_.Id) $($_.MainWindowTitle)" } | Set-Content C:\S10eTmp\title.txt
