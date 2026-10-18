# iddstate.ps1 - G18 remote-monitor PoC state: the S10eIdd (IddSampleDriver) device + bound driver version, and whether
# the frame-sink section Global\S10eIddFrame exists (header fields). Run as admin/SYSTEM. notes/g18-remote-monitors-research.md.
foreach ($d in Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -like '*Idd*' }) {
  $v = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
  $i = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
  "device '{0}' {1} driver {2} ({3})" -f $d.FriendlyName, $d.Status, $v, $i
}
try {
  $m = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting('Global\S10eIddFrame', [System.IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
  $a = $m.CreateViewAccessor(0, 32, [System.IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
  "section: magic 0x{0:X8} {1}x{2} pitch {3} seq {4} format {5} render-luid-low 0x{6:X} nonzero {7}" -f $a.ReadUInt32(0), $a.ReadUInt32(4), $a.ReadUInt32(8), $a.ReadUInt32(12), $a.ReadInt32(16), $a.ReadUInt32(20), $a.ReadUInt32(24), $a.ReadUInt32(28)
Get-CimInstance Win32_VideoController | ForEach-Object { "adapter '$($_.Name)' $($_.PNPDeviceID)" }
} catch { "section: $($_.Exception.InnerException.Message)$($_.Exception.Message)" }
