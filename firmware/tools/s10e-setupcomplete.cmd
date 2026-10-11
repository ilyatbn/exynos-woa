@echo off
rem Windows Setup runs this as SYSTEM at the end of installation (C:\Windows\Setup\Scripts\SetupComplete.cmd).
rem It replays, inside Windows, what used to need an elevated PC + offline `reg load` / `bcdedit /store`
rem (s10e-hive-power.ps1, s10e-hive-diag.ps1, the BCD numproc edit) plus the pnputil touch install of
rem extra-touch.cmd. Placed by firmware/tools/s10e-provision-fresh.sh from TWRP. Log: C:\S10eProv\setupcomplete.log
set P=C:\S10eProv
set LOG=%P%\setupcomplete.log
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
if not exist "%TMP%" mkdir "%TMP%"
echo %date% %time% start > "%LOG%"

rem 6 CPUs come from the firmware's parking protocol (mask 0x3E, notes/m14-smp.md); the installer's BCD
rem caps them at 4.
bcdedit /deletevalue {default} numproc >> "%LOG%" 2>&1

rem Power (s10e-hive-power.ps1, W37): display/sleep/hibernate never, no lock screen, no hiberboot, full dumps.
for %%G in (3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e 8ec4b3a5-6868-48c2-be75-4f3044be88a7 29f6c1db-86da-48c5-9fdb-f2b67b1f44da 9d7815a6-7ee4-497e-8888-515a05f02364) do (
  reg add "HKLM\SOFTWARE\Policies\Microsoft\Power\PowerSettings\%%G" /v ACSettingIndex /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
  reg add "HKLM\SOFTWARE\Policies\Microsoft\Power\PowerSettings\%%G" /v DCSettingIndex /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
)
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\Personalization" /v NoLockScreen /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Power" /v HibernateEnabled /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power" /v HiberbootEnabled /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" /v CrashDumpEnabled /t REG_DWORD /d 3 /f >> "%LOG%" 2>&1

rem S10eDiag driver + agent + tools (s10e-hive-diag.ps1).
copy /y "%P%\s10ediag\S10eDiag.sys" C:\Windows\System32\drivers\ >> "%LOG%" 2>&1
for %%F in (s10ediag.exe S10eAgent.exe s10erotate.exe s10eperf.exe s10etouchlog.exe s10ereboot.exe) do copy /y "%P%\s10ediag\%%F" C:\Windows\System32\ >> "%LOG%" 2>&1
sc create S10eDiag type= kernel start= system error= ignore binPath= System32\drivers\S10eDiag.sys DisplayName= "S10e diagnostics" >> "%LOG%" 2>&1
sc create S10eAgent type= own start= auto error= ignore binPath= "%%SystemRoot%%\System32\S10eAgent.exe" obj= LocalSystem DisplayName= "S10e diagnostics agent" >> "%LOG%" 2>&1
rem Android partitions must not get drive letters (agent run 1); the agent letters the ESP itself.
reg add "HKLM\SYSTEM\CurrentControlSet\Services\mountmgr" /v NoAutoMount /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1
for %%L in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do mountvol %%L: /D >> "%LOG%" 2>&1
copy /y C:\Windows\System32\s10ereboot.exe "C:\Users\Public\Desktop\Reboot to TWRP.exe" >> "%LOG%" 2>&1

rem Touch stack (extra-touch.cmd): test certificate, then both packages.
certutil -addstore -f Root "%P%\drivers\wdktestcert.cer" >> "%LOG%" 2>&1
certutil -addstore -f TrustedPublisher "%P%\drivers\wdktestcert.cer" >> "%LOG%" 2>&1
pnputil /add-driver "%P%\drivers\Exynos9820Hsi2c\Exynos9820Hsi2c.inf" /install >> "%LOG%" 2>&1
pnputil /add-driver "%P%\drivers\S6SY761TouchBeyond0\S6SY761TouchBeyond0.inf" /install >> "%LOG%" 2>&1

echo %date% %time% done >> "%LOG%"
