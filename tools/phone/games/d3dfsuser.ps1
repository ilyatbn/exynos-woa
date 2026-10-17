# (user session, via run-as-user.ps1) d3dfsuser.ps1 - run d3dfs32.exe (drivers/MaliG76Wddm/tools/d3dfs) with the
# arguments in C:\S10eTmp\d3dfs.args (optional) -> C:\S10eTmp\d3dfs.out. G24 fullscreen orientation
# (notes/g24-compute-cs4x.md).
$p = @{ FilePath = 'C:\S10eTmp\d3dfs\d3dfs32.exe'; WorkingDirectory = 'C:\S10eTmp\d3dfs'; Wait = $true; NoNewWindow = $true
        RedirectStandardOutput = 'C:\S10eTmp\d3dfs.out'; RedirectStandardError = 'C:\S10eTmp\d3dfs.err' }
if (Test-Path C:\S10eTmp\d3dfs.args) {
  $a = (Get-Content C:\S10eTmp\d3dfs.args -Raw).Trim()
  if ($a) { $p.ArgumentList = $a }
}
try { Start-Process @p } catch { $_.Exception.Message | Set-Content C:\S10eTmp\d3dfs.err }
