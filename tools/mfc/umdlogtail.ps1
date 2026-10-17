# umdlogtail.ps1 [-Exe mpv] [-Pattern 'gdi|TEMP'] [-Tail 8] - G17 (notes/g17-dxva-mfc.md): newest UMD trace log
# (C:\S10eTmp\tlog\malikm-<exe>*, machine-wide umdtrace) and its last lines matching -Pattern, plus any TEMP frame
# dumps (frame-*.bgra). Avoids regex quoting through ssh + cmd.
param([string]$Exe = 'mpv', [string]$Pattern = 'gdi|TEMP', [int]$Tail = 8)
$f = Get-ChildItem C:\S10eTmp\tlog -Filter "malikm-$Exe*" | Where-Object Name -notlike '*stderr*' |
     Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $f) { "no malikm-$Exe log"; return }
"$($f.Name) $($f.Length) B $($f.LastWriteTime)"
Get-Content $f.FullName | Select-String -Pattern $Pattern | Select-Object -Last $Tail | ForEach-Object { $_.Line }
Get-ChildItem C:\S10eTmp\tlog -Filter 'frame-*.bgra' | ForEach-Object { "dump $($_.Name) $($_.Length) B" }
