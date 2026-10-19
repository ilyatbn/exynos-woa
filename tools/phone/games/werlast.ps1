# werlast.ps1 [-Match DaveTheDiver] [-Last 2] - the newest WER crash reports (ReportArchive + ReportQueue) of an app:
# time, exception code / module / offset (Sig[]), and the loaded Mali UMD modules. notes/games-unity.md.
param([string]$Match = 'DaveTheDiver', [int]$Last = 2)
$roots = 'C:\ProgramData\Microsoft\Windows\WER\ReportArchive', 'C:\ProgramData\Microsoft\Windows\WER\ReportQueue'
Get-ChildItem $roots -Directory -ErrorAction SilentlyContinue | Where-Object Name -match $Match |
  Sort-Object LastWriteTime | Select-Object -Last $Last | ForEach-Object {
    "== $($_.Name) $($_.LastWriteTime)"
    $w = Join-Path $_.FullName 'Report.wer'
    if (Test-Path $w) {
      Get-Content $w | Where-Object { $_ -match '^(EventTime|Sig\[\d+\]\.(Name|Value)|AppPath)=' -or $_ -match '^LoadedModule.*mali' }
    }
    Get-ChildItem $_.FullName | ForEach-Object { "  file $($_.Name) $($_.Length)" }
  }
