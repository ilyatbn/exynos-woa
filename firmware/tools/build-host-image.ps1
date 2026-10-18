<#
.SYNOPSIS
  Build the S10e "host" boot image (the one in daily use since HANDOFF 56: boot-host-btmfc.img) from the current tree.

.DESCRIPTION
  boot-host-btmfc.img was HEAD + build-only switch overrides that were edited in, built and reverted by hand. This
  script applies exactly those overrides to a build, packs the image and restores the sources byte-for-byte (also on
  failure), so the image is reproducible from a commit:
    GpuProbeDxe  BEYOND0LTE_GPU_POWER_ON 1      BootLogDxe BEYOND0LTE_WINDOWS_WATCHDOG 0 (interactive use)
    BtUartDxe    BEYOND0LTE_BT_UART_PROBE 1     MfcDxe     BEYOND0LTE_MFC_PROBE / _POWER / _FWBOOT 1
    UsbHostDxe   BEYOND0LTE_USB_KD_DEVICE 0 (USB-C host: the keyboard dongle; no KDNET)
  build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG, then pack-uefi-beyond0lte.ps1 -Ref ref-recovery.img
  (HANDOFF 29 recipe). Flash from TWRP with tools/phone/bootflash.sh.

.PARAMETER Output  The image to write (e.g. C:\Users\ilya\code\s10e-artifacts\boot-host-r1.img).
.PARAMETER Ref     Reference boot image sboot accepts (default s10e-artifacts\ref-recovery.img).
.PARAMETER Watchdog 1 = the 82 s watchdog stays armed into Windows (a hang returns the phone to TWRP) - for the first,
                   unattended boot of a risky change; 0 (default) = the daily image.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$Output,
  [string]$Ref = "C:\Users\ilya\code\s10e-artifacts\ref-recovery.img",
  [ValidateSet(0, 1)] [int]$Watchdog = 0
)
$ErrorActionPreference = "Stop"
$fw = Split-Path -Parent $PSScriptRoot
$drv = Join-Path $fw "Platform\Beyond0LtePkg\Drivers"
$overrides = @(
  @{ File = "Beyond0LteGpuProbeDxe\Beyond0LteGpuProbeDxe.c"; Name = "BEYOND0LTE_GPU_POWER_ON"; Value = "1" },
  @{ File = "Beyond0LteBootLogDxe\Beyond0LteBootLogDxe.c"; Name = "BEYOND0LTE_WINDOWS_WATCHDOG"; Value = "$Watchdog" },
  @{ File = "Beyond0LteBtUartDxe\Beyond0LteBtUartDxe.c"; Name = "BEYOND0LTE_BT_UART_PROBE"; Value = "1" },
  @{ File = "Beyond0LteMfcDxe\Beyond0LteMfcDxe.c"; Name = "BEYOND0LTE_MFC_PROBE"; Value = "1" },
  @{ File = "Beyond0LteMfcDxe\Beyond0LteMfcDxe.c"; Name = "BEYOND0LTE_MFC_POWER"; Value = "1" },
  @{ File = "Beyond0LteMfcDxe\Beyond0LteMfcDxe.c"; Name = "BEYOND0LTE_MFC_FWBOOT"; Value = "1" },
  @{ File = "Beyond0LteUsbHostDxe\Beyond0LteUsbHostDxe.c"; Name = "BEYOND0LTE_USB_KD_DEVICE"; Value = "0" }
)
$saved = @{}
try {
  foreach ($o in $overrides) {
    $path = Join-Path $drv $o.File
    if (-not $saved.ContainsKey($path)) { $saved[$path] = [IO.File]::ReadAllBytes($path) }
    $text = [IO.File]::ReadAllText($path)
    $rx = "(?m)^(#define\s+$($o.Name)\s+)\S+"
    if ($text -notmatch $rx) { throw "$($o.Name) not found in $path" }
    $text = [regex]::Replace($text, $rx, "`${1}$($o.Value)")
    [IO.File]::WriteAllText($path, $text)
    "override $($o.Name) = $($o.Value)"
  }
  # PowerShell 5.1 turns a native command's stderr lines into a terminating NativeCommandError under the Stop
  # preference (with 2>&1) - the build writes ordinary progress there. Continue for the native calls; exit codes decide.
  $ErrorActionPreference = "Continue"
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build-uefi.ps1") -Platform Beyond0LtePkg -Target DEBUG 2>&1 |
    ForEach-Object { "$_" } | Select-Object -Last 6
  if ($LASTEXITCODE) { throw "build-uefi failed ($LASTEXITCODE)" }
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "pack-uefi-beyond0lte.ps1") -Ref $Ref -Output $Output 2>&1 |
    ForEach-Object { "$_" } | Select-Object -Last 4
  if ($LASTEXITCODE) { throw "pack failed ($LASTEXITCODE)" }
} finally {
  foreach ($path in $saved.Keys) { [IO.File]::WriteAllBytes($path, $saved[$path]) }
  "sources restored ($($saved.Count) files)"
}
$h = (Get-FileHash $Output -Algorithm SHA256).Hash.ToLower()
"{0}  {1} ({2} bytes)" -f $h, $Output, (Get-Item $Output).Length
