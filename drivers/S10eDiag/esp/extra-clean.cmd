@echo off
rem S10eAgent extra step: hand the phone back for plain interactive use (2026-10-14, notes/gpu-hw-path.md).
rem Removes the machine-wide Mesa trace switch (MALIKM_TGSI_DIR: per-process tlog files, DWM's included), turns
rem kernel debugging off, records what stays (MaliG76Wddm Parameters = the working GPU configuration, WER LocalDumps
rem = passive crash dumps). Takes effect from the next boot. Output: out\last\clean\.
set X=%L%\clean
mkdir "%X%" > nul 2>&1
s10ediag mark x: clean start
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" > "%X%\env-before.txt" 2>&1
reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v MALIKM_TGSI_DIR /f > "%X%\env-delete.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" > "%X%\env-after.txt" 2>&1
bcdedit /enum {current} > "%X%\bcd-before.txt" 2>&1
bcdedit /debug off > "%X%\bcd-off.txt" 2>&1
bcdedit /enum {current} > "%X%\bcd-after.txt" 2>&1
reg query HKLM\SYSTEM\CurrentControlSet\Services\MaliG76Wddm\Parameters > "%X%\mali-params.txt" 2>&1
reg query "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /s > "%X%\localdumps.txt" 2>&1
s10ediag mark x: clean done
s10ediag flush > nul 2>&1
