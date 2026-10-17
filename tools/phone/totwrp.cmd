@echo off
rem totwrp.cmd - clean Windows restart into TWRP from an ssh session (run on the phone as administrator): the same
rem mechanism collect.cmd uses (HANDOFF #113+): \S10EDIAG\torecovery.flg on the ESP, then shutdown /r; BootLogDxe
rem sees the flag on the OS-restart path and boots recovery. A clean restart keeps NTFS clean, so TWRP's ntfs-3g
rem mounts it read-write for driver swaps (memory driver-swap-from-twrp; s10ereboot's flushreset is a hard reset).
rem The ESP is the volume S10eAgent gave a letter (it holds \S10EDIAG\collect.cmd; mountvol /s fails on this firmware).
setlocal
set L=
for %%D in (S T U V W X Y Z D E F G H I J K L M N O P Q R) do if not defined L if exist %%D:\S10EDIAG\collect.cmd set L=%%D
if not defined L (echo no drive holds \S10EDIAG\collect.cmd & exit /b 1)
echo recovery > %L%:\S10EDIAG\torecovery.flg || exit /b 2
echo flag on %L%:
shutdown /r /t 3 /c "totwrp"
echo TOTWRP_OK
