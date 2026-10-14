@echo off
rem S10eAgent extra step for interactive boots (2026-10-14): put the user-facing tools on the Public Desktop.
rem Copy only - nothing runs, no restart. GpuCheck.exe = drivers/MaliG76Wddm/tools/gpucheck (shows that the Mali GPU
rem renders: adapter, first-frame readback, fps, Windows GPU counter, KMD job counters).
copy /y "%ESP%\S10EDIAG\gpu\GpuCheck.exe" "C:\Users\Public\Desktop\GPU Check.exe" > "%L%\desktop-apps.txt" 2>&1
