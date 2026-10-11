@echo off
rem S10eAgent extra step: how much RAM does Windows see (m13, boot #48 publishes the 64-bit bank). notes/m13-ram.md.
s10ediag mark m: memory
wmic computersystem get TotalPhysicalMemory > "%L%\mem-wmic.txt" 2>&1
wmic memphysical get MaxCapacity > "%L%\mem-wmic-max.txt" 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_OperatingSystem | Format-List TotalVisibleMemorySize,FreePhysicalMemory" > "%L%\mem-os.txt" 2>&1
s10ediag mark m: done
