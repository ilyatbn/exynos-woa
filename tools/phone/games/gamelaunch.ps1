# (user session, via run-as-user.ps1) start the game named in C:\S10eTmp\gamelaunch.args ("<exe path>") with its own
# folder as the working directory, then watch it for 20 s: alive / exit code, and its loaded graphics modules every
# 2 s -> C:\S10eTmp\gamelaunch.out. Games bring-up (notes/games-nfsu2.md).
$exe = (Get-Content C:\S10eTmp\gamelaunch.args -Raw).Trim()
$out = 'C:\S10eTmp\gamelaunch.out'
"start $exe at $(Get-Date -Format HH:mm:ss.fff)" | Set-Content $out
$p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
"pid $($p.Id)" | Add-Content $out
for ($i = 0; $i -lt 10; $i++) {
  Start-Sleep -Seconds 2
  if ($p.HasExited) { "exited after ~$(2 * ($i + 1)) s, code 0x{0:X8}" -f $p.ExitCode | Add-Content $out; break }
  try {
    $m = (Get-Process -Id $p.Id).Modules | Where-Object { $_.ModuleName -match '^(d3d|dxgi|maliumd|libgallium|dinput|ddraw|opengl|vulkan|wined3d|dxvk|d3d9on12|d3d12|eax|dsound)' } |
         ForEach-Object ModuleName
    "t+$(2 * ($i + 1)) s alive, ws $([int]((Get-Process -Id $p.Id).WorkingSet64 / 1MB)) MB, modules: $($m -join ' ')" | Add-Content $out
  } catch { "t+$(2 * ($i + 1)) s: $($_.Exception.Message)" | Add-Content $out }
}
