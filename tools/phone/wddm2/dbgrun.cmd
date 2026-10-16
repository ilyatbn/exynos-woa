@echo off
rem dbgrun.cmd SCRIPT EXE [ARGS] - run EXE under the ARM64 cdb copied to C:\S10eTmp\dbg (Debuggers\arm64: cdb.exe,
rem dbgeng, dbghelp, DbgModel, dbgcore, symsrv, srcsrv), with public symbols cached in C:\S10eTmp\sym and the cdb
rem commands from SCRIPT (a $$< command file). Output: stdout. G15 S4 tracked-fence debugging, tools/phone/wddm2.
set _NT_SYMBOL_PATH=srv*C:\S10eTmp\sym*https://msdl.microsoft.com/download/symbols
set SCRIPT=%1
shift
C:\S10eTmp\dbg\cdb.exe -G -lines -c "$$<%SCRIPT%" %1 %2 %3 %4 %5 %6 %7 %8 %9
