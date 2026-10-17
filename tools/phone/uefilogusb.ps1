# uefilogusb.ps1 - the USB lines of this boot's UEFI log (S:\UEFILOG.TXT, the ESP letter S10eAgent assigns): host or
# KD-device image, CC/VBUS, PHY, xHCI. For "my USB-C to A dongle / keyboard does not light up".
$f = 'S:\UEFILOG.TXT'
if (-not (Test-Path $f)) { "no $f"; exit 1 }
(Get-Item $f).LastWriteTime
Select-String -Path $f -Pattern 'USB|Usb|usb|KDNET|VBUS|Vbus|boost|CCIC|CC_STATUS|XHC|xHCI|DWC3|OTG' |
  Select-Object -First 40 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(170, $_.Line.Length)) }
