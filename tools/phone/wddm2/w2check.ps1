$dev = 'ACPI\SAMS9828\0'
"Wddm2 param: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters').Wddm2
pnputil /enum-devices /instanceid $dev | Select-String "Status|Problem"
$tr = "HKLM:\SYSTEM\CurrentControlSet\Enum\$dev\Device Parameters\MaliG76Wddm"
$v = Get-ItemProperty $tr -ErrorAction SilentlyContinue
if ($v) { ($v.PSObject.Properties | Where-Object { $_.Name -match '^(Fail_|LastFailed|Build|Started|GpuUp)' } | ForEach-Object { '{0}=0x{1:x}' -f $_.Name, $_.Value }) -join ' ' }
Get-Process dwm, LogonUI, explorer -ErrorAction SilentlyContinue | ForEach-Object { "{0} pid {1} start {2:T} cpu {3:n1}" -f $_.ProcessName, $_.Id, $_.StartTime, $_.CPU }
$dwm = Get-Process dwm -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dwm) { "dwm modules: " + (($dwm.Modules | Where-Object { $_.ModuleName -match 'maliumd|libgallium|d3d10warp|d3d11' } | ForEach-Object { $_.ModuleName }) -join ',') }
Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000; StartTime=(Get-Date).AddMinutes(-10)} -ErrorAction SilentlyContinue | Select-Object -First 5 | ForEach-Object { $m = $_.Message -split "`n"; "{0:T} {1} | {2} | {3}" -f $_.TimeCreated, $m[0].Trim(), $m[1].Trim(), $m[3].Trim() }
Get-ChildItem C:\S10eTmp\dumps -ErrorAction SilentlyContinue | ForEach-Object { "dump " + $_.Name + " " + $_.Length }
