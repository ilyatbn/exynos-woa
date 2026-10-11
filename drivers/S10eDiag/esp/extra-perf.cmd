@echo off
rem S10eAgent extra step (m15): measure before tuning - storage and memory throughput as Windows sees them.
rem notes/m15-perf.md. The boot clocks come from UEFILOG.TXT ("TouchPower: DVFS ...").
set P=%L%\perf
mkdir "%P%"
s10ediag mark f: winsat disk seq read
winsat disk -seq -read -drive c > "%P%\disk-seq-read.txt" 2>&1
s10ediag mark f: winsat disk ran read
winsat disk -ran -read -drive c > "%P%\disk-ran-read.txt" 2>&1
s10ediag mark f: winsat mem
winsat mem > "%P%\mem.txt" 2>&1
s10ediag mark f: winsat cpu
winsat cpu -compression > "%P%\cpu-compression.txt" 2>&1
s10ediag mark f: done
