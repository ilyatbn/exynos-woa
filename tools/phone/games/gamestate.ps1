# gamestate.ps1 [-Exe SPEED2] [-Pattern <regex>] - is the game alive (CPU, working set), newest crash dumps, and the
# matching lines of its newest UMD tlog (MALIKM_TGSI_DIR = C:\S10eTmp\tlog). Games bring-up (notes/g24-compute-cs4x.md).
param([string]$Exe = 'SPEED2', [string]$Pattern = 'GS .*unsupported|stream output|SetError|assert|Assert')
Get-Process $Exe -ErrorAction SilentlyContinue | Select-Object Id, CPU, @{n = 'MB'; e = { [int]($_.WS / 1MB) } }, Responding | Format-Table -AutoSize
Get-ChildItem C:\S10eTmp\dumps -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 2 Name, LastWriteTime | Format-Table -AutoSize
$t = Get-ChildItem "C:\S10eTmp\tlog\malikm-$Exe*" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
if ($t) {
  "tlog $($t.Name) $($t.Length) B"
  Select-String -Path $t.FullName -Pattern $Pattern | Select-Object -First 40 | ForEach-Object Line
}
