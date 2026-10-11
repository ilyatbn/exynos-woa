@echo off
rem S10eAgent extra step: touch status only (drivers already installed - oem2 Exynos9820Hsi2c, oem4 S6SY761TouchBeyond0).
rem notes/m12-touch.md. Called from collect.cmd with ESP / OUT / L set.
set T=%L%\touch
mkdir "%T%"
s10ediag mark t: status
pnputil /enum-devices /deviceid "ACPI\SCSY0761" > "%T%\dev-touch.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\SAMS9820" > "%T%\dev-hsi2c.txt" 2>&1
pnputil /enum-devices /class HIDClass > "%T%\dev-hid.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SCSY0761\0\Device Parameters" > "%T%\touch-params.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9820\a\Device Parameters" > "%T%\hsi2c-params.txt" 2>&1
s10ediag mark t: status done
