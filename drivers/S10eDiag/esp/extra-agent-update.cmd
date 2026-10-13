@echo off
rem S10eAgent extra step: update S10eAgent.exe / s10ekeys.exe (and other user-mode s10ediag tools) in System32 from
rem %ESP%\S10EDIAG\agent\. S10eAgent.exe is the running service image: rename it to .prev (allowed while running) and
rem copy the new one; it takes effect on the next boot. s10ekeys.exe is replaced directly (started per key press).
set A=%L%\agentupd
mkdir "%A%"
for %%F in (S10eAgent.exe s10ekeys.exe s10ediag.exe s10eperf.exe) do if exist "%ESP%\S10EDIAG\agent\%%F" (
  if exist "%SystemRoot%\System32\%%F.prev" del /f /q "%SystemRoot%\System32\%%F.prev"
  move /y "%SystemRoot%\System32\%%F" "%SystemRoot%\System32\%%F.prev" >> "%A%\move.txt" 2>&1
  copy /y "%ESP%\S10EDIAG\agent\%%F" "%SystemRoot%\System32\%%F" >> "%A%\copy.txt" 2>&1
  certutil -hashfile "%SystemRoot%\System32\%%F" SHA256 >> "%A%\hashes.txt" 2>&1
  s10ediag mark "u: updated %%F"
)
s10ediag mark u: done (S10eAgent takes effect next boot)
