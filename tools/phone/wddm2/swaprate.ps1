# swaprate.ps1 [-Frames 300] - G17 pacing (notes/g17-dxva-mfc.md): Present(1, 0) rate of a DWM-composed swap chain on
# the Mali in the user's session - d3dswap with a composition swap chain (what XAML/WinUI apps such as Photos use)
# and with a plain window swap chain, flip sequential, 2 buffers. ~60/s = DWM composes every vblank.
param([int]$Frames = 300)
foreach ($mode in 'comp', 'hwnd') {
  "--- $mode"
  & C:\S10eTmp\userrun.ps1 -Exe C:\S10eTmp\d3dswap.exe -Args "$Frames $mode seq 2" -Wait 90 |
    Select-String 'mode |done|frame 0 |0x8[0-9a-f]{7}' | ForEach-Object { $_.Line }
}
