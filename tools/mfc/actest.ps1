# actest.ps1 - G17 V6d (notes/g17-dxva-mfc.md): Media Foundation D3D11 decode (mftest -d3d11) inside an
# AppContainer (acrun), the sandbox of packaged players. Test fixture C:\S10eTmp\ac: the tools + clip, readable by
# ALL APPLICATION PACKAGES, and a tlog folder they may write. Prints mftest's result and the UMD's video log (MFC
# session open, or the open failure). (The H.264 profile is exposed natively since 0091.)
# AppContainers cannot start from the SSH session (0xC0000142, no usable window station): -Setup builds the fixture
# (run as administrator over SSH), then run the test itself in the user's session:
#   runuser2.ps1 -Cmd "powershell -File C:\S10eTmp\mfc\actest.ps1 > C:\S10eTmp\ac\out.txt 2>&1"
param([string]$Clip = 'C:\S10eTmp\mfc\c720.mp4', [switch]$Setup)
$ac = 'C:\S10eTmp\ac'
if ($Setup) {
  New-Item -ItemType Directory -Force "$ac\tlog" | Out-Null
  Copy-Item C:\S10eTmp\mfc\mftest2.exe, C:\S10eTmp\mfc\acrun.exe, $Clip $ac -Force
  icacls $ac /grant '*S-1-15-2-1:(OI)(CI)RX' /T /Q | Out-Null
  icacls "$ac\tlog" /grant '*S-1-15-2-1:(OI)(CI)M' /grant '*S-1-5-11:(OI)(CI)M' /T /Q | Out-Null
  icacls $ac /grant '*S-1-5-11:(OI)(CI)M' /Q | Out-Null   # the user-session run writes out.txt here
  'fixture ready'
  return
}
Get-ChildItem "$ac\tlog" | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem Env: | Where-Object Name -like 'D3D10UMD_*' | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:MALIKM_TGSI_DIR = "$ac\tlog"
& "$ac\acrun.exe" "$ac\mftest2.exe" "$ac\$(Split-Path $Clip -Leaf)" -d3d11 -q 2>&1 | ForEach-Object { "  $_" }
Remove-Item Env:MALIKM_TGSI_DIR -ErrorAction SilentlyContinue
$f = Get-ChildItem "$ac\tlog" -Filter 'malikm-mftest2*' | Where-Object Name -notlike '*stderr*' | Select-Object -Last 1
if ($f) {
  "UMD log $($f.Name):"
  Get-Content $f.FullName | Select-String 'video|SetError|init' | Where-Object { $_.Line -notmatch 'QueryVideoCapabilities|buffer type|buffer info|config count' } |
    Select-Object -First 25 | ForEach-Object { '    ' + ($_.Line -replace '\s+', ' ') }
} else { 'no UMD log in the container-writable folder' }
