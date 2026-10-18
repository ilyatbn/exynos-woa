# smbiosinfo.ps1 - what Windows reads from our SMBIOS (R2, notes/ram-6gb.md): GetPhysicallyInstalledSystemMemory
# (Task Manager's "installed"), visible memory, Win32_PhysicalMemory / PhysicalMemoryArray, system / enclosure / BIOS.
Add-Type -Name M -Namespace S10eRam -MemberDefinition '[DllImport("kernel32.dll", SetLastError = true)] public static extern bool GetPhysicallyInstalledSystemMemory(out ulong kb);'
$kb = 0; $ok = [S10eRam.M]::GetPhysicallyInstalledSystemMemory([ref]$kb)
"GetPhysicallyInstalledSystemMemory: ok $ok  $kb KB = $([math]::Round($kb / 1MB, 2)) GiB  (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
$os = Get-CimInstance Win32_OperatingSystem
"TotalVisibleMemorySize: $($os.TotalVisibleMemorySize) KB = $([math]::Round($os.TotalVisibleMemorySize / 1MB, 2)) GiB"
Get-CimInstance Win32_PhysicalMemory | ForEach-Object { "PhysicalMemory: $($_.Capacity) B, locator '$($_.DeviceLocator)' bank '$($_.BankLabel)' type $($_.SMBIOSMemoryType)" }
Get-CimInstance Win32_PhysicalMemoryArray | ForEach-Object { "PhysicalMemoryArray: max $($_.MaxCapacityEx) KB, devices $($_.MemoryDevices)" }
$cs = Get-CimInstance Win32_ComputerSystem; "ComputerSystem: '$($cs.Manufacturer)' '$($cs.Model)' family '$($cs.SystemFamily)' sku '$($cs.SystemSKUNumber)'"
"Enclosure: types $((Get-CimInstance Win32_SystemEnclosure).ChassisTypes -join ',')"
$b = Get-CimInstance Win32_BIOS; "BIOS: '$($b.Manufacturer)' '$($b.SMBIOSBIOSVersion)' SMBIOS $($b.SMBIOSMajorVersion).$($b.SMBIOSMinorVersion)"
