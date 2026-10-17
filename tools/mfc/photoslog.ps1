# photoslog.ps1 [-Log 40] - G17 V6d (notes/g17-dxva-mfc.md): the newest UMD trace log of the Photos app. With
# machine-wide tracing on (tools/phone/wddm2/umdtrace.ps1) an AppContainer process cannot write C:\S10eTmp\tlog, so
# the shim falls back to the container's temp folder (malikm_drm.c, 0082): look in both.
param([int]$Log = 40)
$o = Get-CimInstance Win32_Process -Filter "name='explorer.exe'" | Select-Object -First 1 | Invoke-CimMethod -MethodName GetOwner
$dirs = @('C:\S10eTmp\tlog') + @(Get-ChildItem "C:\Users\$($o.User)\AppData\Local\Packages" -Directory -Filter 'Microsoft.Windows.Photos_*' -ErrorAction SilentlyContinue |
  ForEach-Object { Join-Path $_.FullName 'AC\Temp' })
$f = $dirs | ForEach-Object { Get-ChildItem $_ -Filter 'malikm-Photos*' -ErrorAction SilentlyContinue } |
  Where-Object Name -notlike '*stderr*' | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $f) { "no malikm-Photos log in: $($dirs -join ', ')"; return }
"$($f.FullName) $($f.Length) B $($f.LastWriteTime)"
Get-Content $f.FullName | Select-String 'video|SetError' |
  Where-Object { $_.Line -notmatch 'QueryVideoCapabilities|buffer type|buffer info|config count' } |
  Select-Object -First $Log | ForEach-Object { '    ' + ($_.Line -replace '\s+', ' ') }
