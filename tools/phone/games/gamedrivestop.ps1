# gamedrivestop.ps1 - stop a running gamedrive.ps1 (its key replay) and the game it started (the exe on the "start"
# line of C:\S10eTmp\gamedrive.out). nfsrace.sh calls it when gamedrive started the wrong game (notes/g34-fl11.md).
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'gamedrive\.ps1' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "stopped gamedrive pid $($_.ProcessId)" }
$first = Get-Content C:\S10eTmp\gamedrive.out -TotalCount 1 -ErrorAction SilentlyContinue
if ($first -match '^start (.+\.exe) ') {
  $name = [IO.Path]::GetFileNameWithoutExtension($Matches[1])
  Get-Process $name -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force; "stopped $name pid $($_.Id)" }
}
