$t = (Get-Date).AddMinutes(-6)
Get-Process dwm, LogonUI, explorer -ErrorAction SilentlyContinue | ForEach-Object { "{0} pid {1} start {2:T} cpu {3:n1}" -f $_.ProcessName, $_.Id, $_.StartTime, $_.CPU }
$dwm = Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dwm) { "dwm modules: " + (($dwm.Modules | Where-Object { $_.ModuleName -match 'maliumd|d3d10|d3d11|dxgi|libgallium' } | ForEach-Object { $_.ModuleName }) -join ',') }
Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$t} -ErrorAction SilentlyContinue | Where-Object { $_.LevelDisplayName -ne 'Information' } | Select-Object -First 8 | ForEach-Object { "{0:T} {1} {2}: {3}" -f $_.TimeCreated, $_.ProviderName, $_.Id, (($_.Message -split "`n")[0]) }
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$t} -ErrorAction SilentlyContinue | Where-Object { $_.LevelDisplayName -ne 'Information' } | Select-Object -First 8 | ForEach-Object { "{0:T} {1} {2}: {3}" -f $_.TimeCreated, $_.ProviderName, $_.Id, (($_.Message -split "`n")[0]) }
