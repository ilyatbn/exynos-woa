# D4c rung R5 probe (notes/bt-driver.md): radio state through the Windows Radio API, then a search for nearby
# unpaired Bluetooth devices (LE + classic). Any device found proves inquiry results / LE advertising reports reach
# Windows through the Exynos9820Bt transport. Output: plain text on stdout (extra-bt-scan.cmd redirects it).
param([int]$Seconds = 30)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($Op, [Type]$ResultType) {
    $task = $asTaskGeneric.MakeGenericMethod($ResultType).Invoke($null, @($Op))
    if (-not $task.Wait(60000)) { throw "timeout" }
    $task.Result
}

[Windows.Devices.Radios.Radio, Windows.System.Devices, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null

"=== radios"
try {
    $radios = Await ([Windows.Devices.Radios.Radio]::GetRadiosAsync()) ([System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]])
    $radios | ForEach-Object { "{0} kind={1} state={2}" -f $_.Name, $_.Kind, $_.State }
} catch { "radios failed: $_" }

"=== adapter"
try {
    [Windows.Devices.Bluetooth.BluetoothAdapter, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null
    $a = Await ([Windows.Devices.Bluetooth.BluetoothAdapter]::GetDefaultAsync()) ([Windows.Devices.Bluetooth.BluetoothAdapter])
    "address present {0} LE {1} classic {2} central {3} advertisementOffload {4}" -f ($a.BluetoothAddress -ne 0), $a.IsLowEnergySupported,
        $a.IsClassicSupported, $a.IsCentralRoleSupported, $a.IsAdvertisementOffloadSupported
} catch { "adapter failed: $_" }

function Find($Label, $Selector) {
    # PowerShell 5.1 cannot subscribe to WinRT events (DeviceWatcher.Added), so use FindAllAsync: for an unpaired
    # Bluetooth selector it runs the inquiry / LE scan itself and returns what it found.
    "=== $Label (FindAllAsync)"
    try {
        $t0 = Get-Date
        $list = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($Selector)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
        "found {0} in {1:N1} s" -f $list.Count, ((Get-Date) - $t0).TotalSeconds
        $list | ForEach-Object { "  {0}" -f $_.Name }
    } catch { "FindAllAsync failed: $_" }
}

Find 'le-unpaired' ([Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelectorFromPairingState($false))
Find 'classic-unpaired' ([Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false))
Start-Sleep -Seconds $Seconds
Find 'le-unpaired-again' ([Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelectorFromPairingState($false))
Find 'classic-unpaired-again' ([Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false))
"=== done"
