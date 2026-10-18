# umdwhich.ps1 [-Names dwm,explorer,mpv] - which maliumd.dll each process has loaded (path, SHA-256 prefix, file
# time): DWM loads the UMD at boot from wherever the driver points, which may not be the System32 copy umdswap replaces.
param([string[]]$Names = @('dwm', 'explorer', 'mpv', 'msedge'))
foreach ($n in $Names) {
  foreach ($p in Get-Process $n -ErrorAction SilentlyContinue) {
    $m = $p.Modules | Where-Object { $_.ModuleName -match '^(maliumd(_arm64|_ec)?|libgallium_d3d10)\.dll$' }
    foreach ($x in $m) {
      $h = (Get-FileHash $x.FileName -Algorithm SHA256).Hash.Substring(0, 8).ToLower()
      '{0,-10} {1,6} {2} sha {3} {4:yyyy-MM-dd HH:mm}' -f $n, $p.Id, $x.FileName, $h, (Get-Item $x.FileName).LastWriteTime
    }
  }
}
