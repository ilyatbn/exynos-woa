@echo off
rem S10eAgent extra step: D10 V3 - decode the test clips on the hardware decoder with mfcdec.exe (Exynos9820Mfc
rem IOCTLs) and record the per-frame hashes (compare with tools/mfc/mkclip.py output, notes/d10-mfc-decode-flow.md).
rem Needs C:\S10eTmp\mfc\{mfcdec.exe,cbase.h264,c720.h264,c1080.h264} (staged from TWRP). Also records sshd state
rem (ssh resets during the banner exchange on 2026-10-15).
set P=%L%\mfcdec
mkdir "%P%"
set D=C:\S10eTmp\mfc
for %%C in (cbase c720 c1080) do (
  s10ediag mark d: %%C
  "%D%\mfcdec.exe" "%D%\%%C.h264" > "%P%\%%C.txt" 2>&1
  for /f "usebackq delims=" %%L in (`findstr /b "done: header: error: IOCTL" "%P%\%%C.txt"`) do s10ediag mark "d %%C: %%L"
)
s10ediag mark d: sshd state
sc query sshd > "%P%\sshd.txt" 2>&1
netstat -ano | findstr ":22 " >> "%P%\sshd.txt" 2>&1
if exist C:\ProgramData\ssh\logs xcopy /y /i C:\ProgramData\ssh\logs "%P%\sshlogs" > nul 2>&1
wevtutil qe OpenSSH/Operational /c:40 /rd:true /f:text > "%P%\ssh-events.txt" 2>&1
s10ediag mark d: done
