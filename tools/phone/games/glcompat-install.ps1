# (user session, via runapp.sh) glcompat-install.ps1 - install Microsoft's "OpenCL, OpenGL, and Vulkan Compatibility
# Pack" (Store 9NQPSL29BFFF: Mesa GL/CL on D3D12; without a D3D12 GPU it runs on WARP) for OpenGL games (Stardew
# Valley / MonoGame DesktopGL needs FBOs; notes/games-steam.md). Output -> C:\S10eTmp\glcompat.out.
$o = 'C:\S10eTmp\glcompat.out'
"start $(Get-Date -Format HH:mm:ss)" | Set-Content $o
winget install --id 9NQPSL29BFFF --source msstore --accept-package-agreements --accept-source-agreements --silent 2>&1 | Out-File -Append $o
"winget exit $LASTEXITCODE" | Add-Content $o
Get-AppxPackage *OpenCL* , *GLCompat* , *D3DMappingLayers* -ErrorAction SilentlyContinue | ForEach-Object { "pkg $($_.Name) $($_.Version)" } | Add-Content $o
