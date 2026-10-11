@echo off
rem S10eAgent extra step (m15): measure before tuning. notes/m15-perf.md. winsat printed only its banner as
rem SYSTEM (run 21), so s10eperf.exe (drivers/S10eDiag/S10ePerf.c) measures cpu / mem / disk itself.
set P=%L%\perf
mkdir "%P%"
if not exist C:\S10eTmp mkdir C:\S10eTmp
s10ediag mark f: s10eperf cpu mem
s10eperf cpu mem > "%P%\cpu-mem.txt" 2>&1
s10ediag mark f: s10eperf disk
s10eperf disk C:\S10eTmp > "%P%\disk.txt" 2>&1
s10ediag mark f: done
