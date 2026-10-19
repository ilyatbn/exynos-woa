# steamprocs.ps1 - Steam client and game processes: name, pid, parent pid (+ parent name), working set MB, command line.
# Answers "who started Steam" when a game is launched from a desktop shortcut without Steam (Steamworks games
# relaunch through Steam unless steam_appid.txt sits next to the exe). notes/games-steam.md.
$all = Get-CimInstance Win32_Process
$byId = @{}; foreach ($p in $all) { $byId[$p.ProcessId] = $p }
$all | Where-Object { $_.Name -match 'steam|Dave|Stardew|Schedule' } | ForEach-Object {
  $par = $byId[$_.ParentProcessId]
  '{0} pid {1} parent {2} ({3}) {4} MB  {5}' -f $_.Name, $_.ProcessId, $_.ParentProcessId,
    $(if ($par) { $par.Name } else { 'gone' }), [int]($_.WorkingSetSize / 1MB), $_.CommandLine
}
