@echo off
rem S10eAgent extra step (m14 SMP): how many CPUs does Windows run, and was the previous restart clean?
rem notes/m14-smp.md.
s10ediag mark s: cpu count
wmic cpu get NumberOfCores,NumberOfLogicalProcessors > "%L%\cpu-wmic.txt" 2>&1
wmic computersystem get NumberOfProcessors,NumberOfLogicalProcessors,TotalPhysicalMemory > "%L%\cpu-cs.txt" 2>&1
s10ediag mark s: restart history
wevtutil qe System /q:"*[System[(EventID=41 or EventID=6008 or EventID=1074 or EventID=1001)]]" /c:6 /rd:true /f:text > "%L%\restart-events.txt" 2>&1
s10ediag mark s: done
