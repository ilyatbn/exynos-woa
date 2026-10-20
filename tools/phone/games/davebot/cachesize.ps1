# cachesize.ps1 [-Filter DaveTheDiver*] - G33: the Mali UMD's shader cache directories of the logged-on user
# (%LOCALAPPDATA%\MaliUMD\ShaderCache, here C:\Users\S10e) with file count and size. notes/games-unity.md.
param([string]$Filter = '*')
Get-ChildItem C:\Users\S10e\AppData\Local\MaliUMD\ShaderCache -Directory -Filter $Filter -ErrorAction SilentlyContinue | ForEach-Object {
  $f = Get-ChildItem $_.FullName -Recurse -File
  '{0} files {1} MB {2:N1}' -f $_.Name, $f.Count, (($f | Measure-Object Length -Sum).Sum / 1MB)
}
