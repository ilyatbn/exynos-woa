# sandboxlogs.ps1 [-Tail 8] - stderr files the Mesa UMD writes into the temp dir of processes without a console
# (malikm_drm.c 0082b: AppContainer packages -> Packages\<pkg>\AC\Temp, others -> %TEMP%), newest first.
param([int]$Tail = 8)
$roots = @('C:\Users\S10e\AppData\Local\Packages', 'C:\Users\S10e\AppData\Local\Temp', 'C:\Windows\Temp')
Get-ChildItem $roots -Recurse -Filter 'malikm-*-stderr.txt' -ErrorAction SilentlyContinue |
  Sort-Object LastWriteTime -Descending | Select-Object -First 6 | ForEach-Object {
    "== $($_.FullName) $($_.LastWriteTime)"; Get-Content $_.FullName -Tail $Tail }
