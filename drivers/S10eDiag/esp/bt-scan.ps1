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
    "address {0:X12} LE {1} classic {2} central {3} advertisementOffload {4}" -f $a.BluetoothAddress, $a.IsLowEnergySupported,
        $a.IsClassicSupported, $a.IsCentralRoleSupported, $a.IsAdvertisementOffloadSupported
} catch { "adapter failed: $_" }

function Find($Label, $Selector) {
    "=== $Label (DeviceWatcher, $Seconds s)"
    $found = [System.Collections.Concurrent.ConcurrentDictionary[string, string]]::new()
    $w = [Windows.Devices.Enumeration.DeviceInformation]::CreateWatcher($Selector)
    $null = Register-ObjectEvent -InputObject $w -EventName Added -SourceIdentifier "bt-$Label" -Action {
        $d = $EventArgs; $null = $Event.MessageData.TryAdd($d.Id, $d.Name) } -MessageData $found
    $w.Start()
    Start-Sleep -Seconds $Seconds
    $w.Stop()
    Unregister-Event -SourceIdentifier "bt-$Label" -ErrorAction SilentlyContinue
    "found $($found.Count)"
    $found.GetEnumerator() | ForEach-Object { "  {0} | {1}" -f $_.Value, $_.Key }
}

Find 'le-unpaired' ([Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelectorFromPairingState($false))
Find 'classic-unpaired' ([Windows.Devices.Bluetooth.BluetoothDevice]::GetDeviceSelectorFromPairingState($false))
"=== done"
