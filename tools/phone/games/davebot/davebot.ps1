# (user session, started HIDDEN by davebot-start.ps1) davebot.ps1 - screen-driven Dave the Diver player for benchmarks
# (notes/games-unity.md, user 2026-10-20: "figure out how to play the game ... check the screenshots"). One long-lived
# hidden process: no cmd / console window ever comes up in front of the game (userrun's cmd /c window and the
# screensaver both stole the foreground during earlier runs).
# Request files in C:\S10eTmp\bot (written over SSH, deleted when done):
#   shot.req          -> shot.png (full screen, DPI-aware GDI copy) + shot.txt
#   key.req  "<scan hex> <ext 0|1> <hold ms>"   -> one key press via keyplay
#   tap.req  "<x> <y>"                           -> one touch tap via keyplay
#   launch.req        -> start DaveTheDiver.exe (from this process: no foreign window in front)
#   quit.req          -> exit
# Log: C:\S10eTmp\bot\bot.log
$dir = 'C:\S10eTmp\bot'
New-Item -ItemType Directory -Force $dir | Out-Null
$log = Join-Path $dir 'bot.log'
function L($m) { "{0} {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $m | Add-Content $log }
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Name Dpi -Namespace Bot -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[void][Bot.Dpi]::SetProcessDPIAware()
Add-Type -Namespace W -Name Spi -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint u, System.IntPtr p, uint f);'
[void][W.Spi]::SystemParametersInfo(0x11, 0, [IntPtr]::Zero, 0)   # screen saving off for this session (not persisted)

function Shot([string]$out) {
  Get-Process scrnsave -ErrorAction SilentlyContinue | Stop-Process -Force
  $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
  $g.Dispose()
  return $bmp
}
function Play([string[]]$lines) {
  $f = Join-Path $dir 'one.rec'
  Set-Content $f $lines
  Start-Process -Wait -WindowStyle Hidden -FilePath C:\S10eTmp\keyplay.exe -ArgumentList $f, 100, 'DaveTheDiver.exe'
  L "play: $($lines -join ' | ') -> $(Get-Content C:\S10eTmp\keyplay.log -Tail 1)"
}

# ---- screen states (signature points from reference shots traces/games/bot/*.png, 1520x720 physical; sample.ps1) ----
function Px($b, $x, $y) { $c = $b.GetPixel($x, $y); return @($c.R, $c.G, $c.B) }
function Near($c, $r, $g, $bl, $tol) { return [Math]::Abs($c[0] - $r) -le $tol -and [Math]::Abs($c[1] - $g) -le $tol -and [Math]::Abs($c[2] - $bl) -le $tol }
function State($b) {
  $o1 = Px $b 185 628; $o2 = Px $b 230 628; $o3 = Px $b 207 675          # O2 gauge fill 00aeef
  if ((Near $o1 0 174 239 40) -and (Near $o2 0 174 239 40) -and (Near $o3 0 174 239 40)) { return 'water' }
  $coin = Px $b 195 62; $bar = Px $b 300 62                               # boat HUD: coin e3a103, bar ~050a0a
  if ((Near $coin 227 161 3 40) -and $bar[0] -lt 40 -and $bar[1] -lt 40 -and $bar[2] -lt 40) {
    $d = Px $b 640 490                                                    # DIVE prompt ffe025
    if ($d[0] -gt 220 -and $d[1] -gt 190 -and $d[2] -lt 90) { return 'boat-dive' }
    return 'boat'
  }
  $l = Px $b 560 240                                                      # title logo fef01e
  if ($l[0] -gt 220 -and $l[1] -gt 200 -and $l[2] -lt 90) { return 'title' }
  $flat = $true
  foreach ($p in @(@(300, 300), @(1100, 250), @(600, 450), @(760, 493))) {
    $c = Px $b $p[0] $p[1]
    if (-not ((Near $c 0 0 0 8) -or (Near $c 46 46 46 8))) { $flat = $false }
  }
  if ($flat) { return 'loading' }
  return 'unknown'
}
function Key($scan, $ms) { Play @("100 D $scan 0", "$(100 + $ms) U $scan 0") }
# swim pattern (same as swim.py): right (D) 4 s, down (S) 1.5 s, left (A) 4 s, down 1.5 s, ... for $sec seconds
function SwimRec([int]$sec, [string]$out) {
  $t = 1000; $i = 0; $steps = @(@('20', 4000), @('1F', 1500), @('1E', 4000), @('1F', 1500)); $l = @('# davebot swim')
  while ($t -lt $sec * 1000) { $k = $steps[$i % 4]; $l += "$t D $($k[0]) 0"; $l += "$($t + $k[1]) U $($k[0]) 0"; $t += $k[1] + 200; $i++ }
  Set-Content $out $l
}
function Tap($x, $y) { Play @("100 M LD $x $y", "100 M LU $x $y") }

# auto.req "<swim seconds>": launch if needed, then play by what is on screen: title -> tap Continue; loading -> wait;
# boat -> D in 400 ms steps until the DIVE prompt, then hold Space; water -> frame times on (Mesa 0168 flag) + the swim
# pattern (swim.rec), off, close the game. Progress lines "auto: ..." in bot.log, "auto done" / "auto FAIL ..." at the end.
function Auto([int]$swim) {
  $exe = 'C:\Program Files (x86)\Steam\steamapps\common\Dave the Diver\DaveTheDiver.exe'
  if (-not (Get-Process DaveTheDiver -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe); L "auto: launched"
  }
  $t0 = Get-Date; $last = ''; $since = Get-Date; $titleTaps = 0; $steps = 0; $spaced = $null; $unk = 0
  while (((Get-Date) - $t0).TotalMinutes -lt 15) {
    if (-not (Get-Process DaveTheDiver -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -gt 60) { L "auto FAIL game gone"; return }
    $b = Shot; $s = State $b
    if ($s -ne $last) { L "auto: state $s"; $last = $s; $since = Get-Date }
    $held = ((Get-Date) - $since).TotalSeconds
    switch ($s) {
      'title' { if ($held -ge 5 -and ($titleTaps -eq 0 -or $held -ge 20 * $titleTaps)) { Tap 760 493; $titleTaps++ } }
      'boat' { if ($spaced -and ((Get-Date) - $spaced).TotalSeconds -lt 15) { break }
               if ($held -ge 3) { if ($steps -lt 20) { Key '20' 400; $steps++ } else { Key '1E' 3000; $steps = 0 } } }
      'boat-dive' { Key '39' 3000; $spaced = Get-Date }
      'water' {
        if ($held -lt 3) { break }
        Remove-Item C:\S10eTmp\frametimes.txt -ErrorAction SilentlyContinue
        New-Item -ItemType File C:\S10eTmp\frametimes.on -Force | Out-Null
        SwimRec $swim "$dir\swim.rec"
        L "auto: measuring ($swim s swim)"
        Start-Process -Wait -WindowStyle Hidden -FilePath C:\S10eTmp\keyplay.exe -ArgumentList "$dir\swim.rec", 100, 'DaveTheDiver.exe'
        L "auto: swim done -> $(Get-Content C:\S10eTmp\keyplay.log -Tail 1)"
        Remove-Item C:\S10eTmp\frametimes.on -ErrorAction SilentlyContinue
        Start-Sleep 3
        $b.Save("$dir\auto-water-end.png", [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
        Get-Process DaveTheDiver -ErrorAction SilentlyContinue | Stop-Process -Force
        L "auto done"; return
      }
      'unknown' { if ($held -ge 10 -and $unk -lt 20 -and [int]$held % 10 -eq 0) { $b.Save("$dir\unk-$unk.png", [System.Drawing.Imaging.ImageFormat]::Png); $unk++ } }
    }
    $b.Dispose()
    Start-Sleep -Milliseconds 700
  }
  L "auto FAIL timeout in state $last"
}

L "davebot start pid $PID"
while ($true) {
  if (Test-Path "$dir\quit.req") { Remove-Item "$dir\quit.req"; break }
  if (Test-Path "$dir\shot.req") {
    Remove-Item "$dir\shot.req"
    $bmp = Shot
    $bmp.Save("$dir\shot.png", [System.Drawing.Imaging.ImageFormat]::Png)
    "$($bmp.Width)x$($bmp.Height) $(Get-Date -Format HH:mm:ss.fff)" | Set-Content "$dir\shot.txt"
    $bmp.Dispose()
  }
  if (Test-Path "$dir\key.req") {
    $a = (Get-Content "$dir\key.req" -Raw).Trim() -split '\s+'; Remove-Item "$dir\key.req"
    Play @("100 D $($a[0]) $($a[1])", "$(100 + [int]$a[2]) U $($a[0]) $($a[1])")
  }
  if (Test-Path "$dir\auto.req") {
    $a = (Get-Content "$dir\auto.req" -Raw).Trim(); Remove-Item "$dir\auto.req"
    Auto ([int]$(if ($a -match '^\d+$') { $a } else { 100 }))
  }
  if (Test-Path "$dir\rec.req") {   # play a whole keyrec file (path inside)
    $p = (Get-Content "$dir\rec.req" -Raw).Trim(); Remove-Item "$dir\rec.req"
    Start-Process -Wait -WindowStyle Hidden -FilePath C:\S10eTmp\keyplay.exe -ArgumentList $p, 100, 'DaveTheDiver.exe'
    L "rec $p -> $(Get-Content C:\S10eTmp\keyplay.log -Tail 1)"
  }
  if (Test-Path "$dir\launch.req") {
    Remove-Item "$dir\launch.req"
    $exe = 'C:\Program Files (x86)\Steam\steamapps\common\Dave the Diver\DaveTheDiver.exe'
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe)
    L "launched"
  }
  if (Test-Path "$dir\tap.req") {
    $a = (Get-Content "$dir\tap.req" -Raw).Trim() -split '\s+'; Remove-Item "$dir\tap.req"
    Play @("100 M LD $($a[0]) $($a[1])", "100 M LU $($a[0]) $($a[1])")
  }
  Start-Sleep -Milliseconds 200
}
[void][W.Spi]::SystemParametersInfo(0x11, 1, [IntPtr]::Zero, 0)
L "davebot exit"
