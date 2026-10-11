@echo off
rem S10eAgent extra step (m16 USB host, notes/m16-usb.md): did UEFI publish XHC0 (USB status word 'USBH' at
rem 0x8FE0F008), did Windows bind inbox USBXHCI to ACPI\PNP0D10, and what enumerated behind the dock's hub.
set P=%L%\usb
mkdir "%P%"
s10ediag mark x: usb status word
s10ediag peek 0x8FE0F008 > "%P%\usbstatus.txt" 2>&1
s10ediag mark x: pnputil usb
pnputil /enum-devices /connected /class USB > "%P%\pnp-usb.txt" 2>&1
pnputil /enum-devices /connected /class HIDClass > "%P%\pnp-hid.txt" 2>&1
pnputil /enum-devices /connected /class Keyboard > "%P%\pnp-kbd.txt" 2>&1
pnputil /enum-devices /connected /class Mouse > "%P%\pnp-mouse.txt" 2>&1
pnputil /enum-devices /connected /class Net > "%P%\pnp-net.txt" 2>&1
pnputil /enum-devices /instanceid "ACPI\PNP0D10\0" /properties > "%P%\xhc0.txt" 2>&1
pnputil /enum-devices /problem > "%P%\pnp-problem.txt" 2>&1
s10ediag mark x: done
