param([string]$Out = "$env:TEMP\camshot.jpg", [double]$ExposureMs = 0)
# One JPEG from the integrated (RGB) camera via WinRT MediaCapture.
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$m = [System.WindowsRuntimeSystemExtensions].GetMethods()
$asOp = ($m | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
$asAct = ($m | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' })[0]
function Await($op, [Type]$t) { $task = $asOp.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }
function AwaitA($a) { $asAct.Invoke($null, @($a)).Wait(-1) | Out-Null }
[Windows.Media.Capture.MediaCapture, Windows.Media.Capture, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Capture.MediaCaptureInitializationSettings, Windows.Media.Capture, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
[Windows.Storage.StorageFolder, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.MediaProperties.ImageEncodingProperties, Windows.Media.MediaProperties, ContentType = WindowsRuntime] | Out-Null
$devs = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync([Windows.Devices.Enumeration.DeviceClass]::VideoCapture)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
$dev = $devs | Where-Object { $_.Name -notmatch 'IR' } | Select-Object -First 1
"camera: $($dev.Name)"
$s = New-Object Windows.Media.Capture.MediaCaptureInitializationSettings
$s.VideoDeviceId = $dev.Id
$s.StreamingCaptureMode = [Windows.Media.Capture.StreamingCaptureMode]::Video
$mc = New-Object Windows.Media.Capture.MediaCapture
AwaitA ($mc.InitializeAsync($s))
# -ExposureMs n: manual exposure (the lit phone in a dark room saturates under auto exposure); 0 = auto
if ($ExposureMs -gt 0) {
    $ec = $mc.VideoDeviceController.ExposureControl
    if ($ec.Supported) {
        AwaitA ($ec.SetAutoAsync($false))
        $t = [TimeSpan]::FromMilliseconds($ExposureMs)
        if ($t -lt $ec.Min) { $t = $ec.Min }
        if ($t -gt $ec.Max) { $t = $ec.Max }
        AwaitA ($ec.SetValueAsync($t))
        "exposure $($t.TotalMilliseconds) ms (range $($ec.Min.TotalMilliseconds)..$($ec.Max.TotalMilliseconds))"
    } else { "exposure control not supported" }
}
Start-Sleep -Milliseconds 1500   # let exposure settle
$dir = Split-Path $Out; $name = Split-Path $Out -Leaf
$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($dir)) ([Windows.Storage.StorageFolder])
$file = Await ($folder.CreateFileAsync($name, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])
AwaitA ($mc.CapturePhotoToStorageFileAsync([Windows.Media.MediaProperties.ImageEncodingProperties]::CreateJpeg(), $file))
$mc.Dispose()
"saved $Out"
