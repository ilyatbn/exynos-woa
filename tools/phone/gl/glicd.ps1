# glicd.ps1 [-Install | -Remove] [-Name maligl.dll] - register / unregister the Mali OpenGL ICD (notes/gl-icd.md) in
# the display adapter's class key, where MS Learn "Loading an OpenGL Installable Client Driver" says opengl32 finds it
# (D3DKMTQueryAdapterInfo KMTQAITYPE_UMOPENGLINFO): OpenGLDriverName REG_SZ, OpenGLVersion REG_DWORD, OpenGLFlags
# REG_DWORD (bit 0 "set for compatibility"). The DLL itself goes to System32 next to maliumd.dll (copied separately).
# The adapter is the class key whose UserModeDriverName names maliumd.dll. No switch: show the current values.
# Testing aid until the KMD INF carries the same values (GL6).
param([switch]$Install, [switch]$Remove, [string]$Name = 'maligl.dll')
$class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
$key = Get-ChildItem $class -ErrorAction SilentlyContinue | Where-Object {
  $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
  $p.UserModeDriverName -and (@($p.UserModeDriverName) -contains 'maliumd.dll')
} | Select-Object -First 1
if (-not $key) { 'no adapter with UserModeDriverName maliumd.dll'; exit 1 }
if ($Install) {
  if (-not (Test-Path "$env:WINDIR\System32\$Name")) { "warning: $env:WINDIR\System32\$Name does not exist" }
  Set-ItemProperty $key.PSPath -Name OpenGLDriverName -Value $Name -Type String
  Set-ItemProperty $key.PSPath -Name OpenGLVersion -Value 1 -Type DWord
  Set-ItemProperty $key.PSPath -Name OpenGLFlags -Value 1 -Type DWord
}
if ($Remove) {
  foreach ($v in 'OpenGLDriverName', 'OpenGLVersion', 'OpenGLFlags') {
    Remove-ItemProperty $key.PSPath -Name $v -ErrorAction SilentlyContinue
  }
}
$p = Get-ItemProperty $key.PSPath
"{0}: OpenGLDriverName '{1}' OpenGLVersion {2} OpenGLFlags {3}" -f $key.PSChildName, $p.OpenGLDriverName, $p.OpenGLVersion, $p.OpenGLFlags
