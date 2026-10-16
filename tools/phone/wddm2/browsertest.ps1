# browsertest.ps1 [-Exe msedge] [-Url about:blank] [-Wait 25] [-Flags ""] [-UmdLog] - G16: start a Chromium browser in
# the user session, then report its GPU process (alive? command line flags), crashes of the run and the GPU lines of
# the browser's own log (chrome_debug.log in the profile, copied to C:\S10eTmp\browser.log). -UmdLog: the browser
# runs with MALIKM_TGSI_DIR=C:\S10eTmp\tlog and --disable-gpu-sandbox so the Mesa UMD in the GPU process can write its log.
param([string]$Exe = 'msedge', [string]$Url = 'about:blank', [int]$Wait = 25, [string]$Flags = '', [switch]$UmdLog)
$t0 = Get-Date
Get-Process $Exe -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 2
$path = (Get-Command "$Exe.exe" -ErrorAction SilentlyContinue).Source
if (-not $path) {
  $cands = @("C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", "C:\Program Files\BraveSoftware\Brave-Browser\Application\brave.exe",
             "C:\Program Files\Google\Chrome\Application\chrome.exe")
  $path = $cands | Where-Object { (Test-Path $_) -and ($_ -like "*$Exe*") } | Select-Object -First 1
}
"browser: $path"
$pre = ''
if ($UmdLog) { $pre = 'set MALIKM_TGSI_DIR=C:\S10eTmp\tlog&& '; $Flags = "--disable-gpu-sandbox $Flags" }
& C:\S10eTmp\runuser2.ps1 -Cmd "${pre}start `"`" `"$path`" --no-first-run --enable-logging --v=0 $Flags $Url" -Wait 10 | Out-Null
Start-Sleep $Wait
$procs = Get-CimInstance Win32_Process -Filter "Name='$Exe.exe'" -ErrorAction SilentlyContinue
"processes: $(@($procs).Count)"
$procs | Where-Object { $_.CommandLine -match '--type=gpu-process' } | ForEach-Object {
  $cl = $_.CommandLine
  "GPU process pid $($_.ProcessId): " + (($cl -split ' ' | Where-Object { $_ -match 'use-angle|use-gl|disable-|enable-' }) -join ' ')
}
if (-not ($procs | Where-Object { $_.CommandLine -match '--type=gpu-process' })) { "GPU process: none" }
Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000; StartTime=$t0} -ErrorAction SilentlyContinue | ForEach-Object { $m = $_.Message -split "`n"; "crash: {0} | {1} | {2}" -f $m[0].Trim(), $m[1].Trim(), $m[3].Trim() }
Get-ChildItem C:\Users\*\AppData\Local\*\*\User?Data\chrome_debug.log -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1 | ForEach-Object {
  "log: $($_.FullName) (copy: C:\S10eTmp\browser.log)"
  Copy-Item $_.FullName C:\S10eTmp\browser.log -Force
  Select-String -Path $_.FullName -Pattern 'gpu|GPU|ANGLE|D3D11|DirectComposition|DComp|swap|fallback|SwiftShader|crash|FATAL' | Select-Object -Last 25 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
}
if ($UmdLog) { Get-ChildItem "C:\S10eTmp\tlog\malikm-$Exe-*" -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $t0 } | ForEach-Object { "umd log: $($_.Name) $($_.Length)" } }
