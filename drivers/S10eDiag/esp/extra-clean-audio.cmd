@echo off
rem S10eAgent extra step: second hand-back boot (2026-10-14, notes/gpu-hw-path.md). noaudio.flg removed from the ESP,
rem so AUD0 is visible with the GPU driver for the first time: record that this boot reached the desktop with both,
rem set the speaker path (audio stage D = AmpInit/AmpEnable/AboxEnable 1, read at PrepareHardware -> next boot) and
rem record the audio device and driver state. Output: out\last\clean\.
set X=%L%\clean
mkdir "%X%" > nul 2>&1
s10ediag mark x: clean-audio start
reg query "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" > "%X%\audio-params-before.txt" 2>&1
call "%ESP%\S10EDIAG\audio-stage-D.cmd"
reg query "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Audio\Parameters" > "%X%\audio-params-after.txt" 2>&1
pnputil /enum-devices /class MEDIA > "%X%\pnp-media.txt" 2>&1
pnputil /enum-devices /problem > "%X%\pnp-problem.txt" 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Audio-run.txt" "%X%\audio-run.txt" > nul 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" > "%X%\env.txt" 2>&1
s10ediag mark x: clean-audio done
s10ediag flush > nul 2>&1
