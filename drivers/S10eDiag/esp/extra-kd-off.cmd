@echo off
rem S10eAgent extra step: turn Windows kernel debugging back off (undo of extra-kd.cmd). notes/m17-usb-kd.md.
set T=%L%\kd
mkdir "%T%" > nul 2>&1
s10ediag mark k: debug off
bcdedit /debug off > "%T%\bcd-off.txt" 2>&1
bcdedit /enum {current} >> "%T%\bcd-off.txt" 2>&1
