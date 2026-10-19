# (user session, started detached by davebench.sh) davebench.ps1 - replay the user's Dave the Diver route (notes/games-unity.md)
# and record per-frame present times during the dive. Segments (C:\S10eTmp\dave-seg1..3.rec, made from the user's keyrec
# recording by davebench.sh, times rebased) start on markers in the game's Player.log, not on fixed times - load times
# vary with RAM pressure:
#   seg1 (title taps)   after "DR.Title.<Start>" and 6 s of log silence
#   seg2 (boat -> dive) after "BGM/BGM_Lobby" and 6 s of silence
#   seg3 (diving)       after "ondestroy called singleton LobbyMainCanvas" and 6 s of silence; frame times are recorded
#                       (C:\S10eTmp\frametimes.on -> the UMD appends to frametimes.txt, Mesa 0168) for the whole segment
# Then the game is closed. Progress in C:\S10eTmp\davebench.log ("done" at the end).
$log = 'C:\S10eTmp\davebench.log'
function L($m) { "{0} {1}" -f (Get-Date -Format 'HH:mm:ss'), $m | Add-Content $log }
Set-Content $log "start"
Remove-Item C:\S10eTmp\frametimes.on, C:\S10eTmp\frametimes.txt -ErrorAction SilentlyContinue
$o = Get-CimInstance Win32_Process -Filter "name='explorer.exe'" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner
$pl = "C:\Users\$($o.User)\AppData\LocalLow\nexon\DAVE THE DIVER\Player.log"
$exe = 'C:\Program Files (x86)\Steam\steamapps\common\Dave the Diver\DaveTheDiver.exe'
Get-Process DaveTheDiver -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
# The phone clock restarts at every boot, so an old Player.log can look newer than this launch: move it aside, only the
# new session's markers count.
if (Test-Path $pl) { Move-Item $pl "$pl.bench-old" -Force }
$t0 = Get-Date
Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
L "game started"

function WaitMarker($marker, $quiet, $timeout) {
  $start = Get-Date
  while (((Get-Date) - $start).TotalSeconds -lt $timeout) {
    if ((Test-Path $pl) -and (Select-String -Path $pl -SimpleMatch $marker -Quiet)) {
      $len = (Get-Item $pl).Length
      $stable = Get-Date
      while (((Get-Date) - $stable).TotalSeconds -lt $quiet -and ((Get-Date) - $start).TotalSeconds -lt $timeout) {
        Start-Sleep -Milliseconds 500
        $n = (Get-Item $pl).Length
        if ($n -ne $len) { $len = $n; $stable = Get-Date }
      }
      return $true
    }
    if (-not (Get-Process DaveTheDiver -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -gt 30) { return $false }
    Start-Sleep 1
  }
  return $false
}

$segs = @(
  @{ File = 'C:\S10eTmp\dave-seg1.rec'; Marker = 'DR.Title.<Start>'; Timeout = 600; Quiet = 10 },   # main menu up (Steam relaunch first)
  @{ File = 'C:\S10eTmp\dave-seg2.rec'; Marker = 'BGM/BGM_Lobby'; Timeout = 600 },
  @{ File = 'C:\S10eTmp\dave-seg3.rec'; Marker = 'ondestroy called singleton LobbyMainCanvas'; Timeout = 600 }
)
$i = 0
foreach ($s in $segs) {
  $i++
  $q = if ($s.Quiet) { $s.Quiet } else { 6 }
  if (-not (WaitMarker $s.Marker $q $s.Timeout)) { L "seg$i marker '$($s.Marker)' not reached - abort"; break }
  L "seg$i marker reached"
  if ($i -eq 3) { New-Item -ItemType File C:\S10eTmp\frametimes.on -Force | Out-Null }
  Start-Process -Wait -FilePath C:\S10eTmp\keyplay.exe -ArgumentList $s.File, 100, 'DaveTheDiver.exe'
  L "seg$i replayed: $(Get-Content C:\S10eTmp\keyplay.log -Tail 1)"
  if ($i -eq 3) {
    Remove-Item C:\S10eTmp\frametimes.on -ErrorAction SilentlyContinue
    Start-Sleep 3   # the UMD closes frametimes.txt within 30 presents
  }
}
Get-Process DaveTheDiver -ErrorAction SilentlyContinue | Stop-Process -Force
L "done"
