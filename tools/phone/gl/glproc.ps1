# glproc.ps1 [-Name "Stardew Valley"] - is the process alive (working set, CPU, window title) and which OpenGL modules
# it has loaded: opengl32 + the Mali ICD (maligl*.dll) = native GL on the Mali; opengl32 alone = GDI Generic (no ICD);
# OpenGLOn12 / d3d12 = the compat pack (notes/gl-icd.md). Exits 0 while the process runs, 1 when it is gone.
param([string]$Name = 'Stardew Valley')
$p = Get-Process $Name -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "$Name not running"; exit 1 }
"{0} alive: ws {1} MB, cpu {2:N0} s, title [{3}], responding {4}" -f $Name, [int]($p.WS / 1MB), $p.CPU, $p.MainWindowTitle, $p.Responding
$p.Modules | Where-Object { $_.ModuleName -match '^(opengl32|maligl|openglon12|d3d12|glu32|SDL2|libgallium)' } |
  ForEach-Object { "  {0}  {1}" -f $_.ModuleName, $_.FileName }
exit 0
