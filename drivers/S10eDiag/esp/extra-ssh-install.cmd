@echo off
rem S10eAgent extra step (2026-10-14, user: "ssh server is perfect"): install Win32-OpenSSH sshd so the PC can run
rem commands on the phone's Windows over the laptop hotspot (notes/phone-ssh.md). Idempotent: skips the install
rem when the sshd service exists, always re-asserts key, config, firewall rule and service state.
rem   - binaries: \S10EDIAG\ssh\OpenSSH-ARM64.zip (PowerShell/Win32-OpenSSH 10.0.0.0p2, Microsoft-signed)
rem   - key only: PasswordAuthentication no; the PC's key (\S10EDIAG\ssh\authorized_keys) goes to
rem     C:\ProgramData\ssh\administrators_authorized_keys (Administrators + SYSTEM only)
rem   - firewall: TCP 22 inbound from 192.168.137.0/24 (the hotspot) only
rem Also copies GPU Check to the Public Desktop (extra-desktop-apps.cmd). Output: out\last\ssh\.
set X=%L%\ssh
mkdir "%X%" > nul 2>&1
set S=%ESP%\S10EDIAG\ssh
set D=C:\Program Files\OpenSSH
s10ediag mark x: ssh install start
copy /y "%ESP%\S10EDIAG\gpu\GpuCheck.exe" "C:\Users\Public\Desktop\GPU Check.exe" > "%X%\desktop-apps.txt" 2>&1
sc query sshd > nul 2>&1
if not errorlevel 1 goto configured
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Force '%S%\OpenSSH-ARM64.zip' '%TEMP%\openssh'; New-Item -ItemType Directory -Force '%D%' | Out-Null; Copy-Item -Force '%TEMP%\openssh\OpenSSH-ARM64\*' '%D%'" > "%X%\expand.txt" 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%D%\install-sshd.ps1" > "%X%\install.txt" 2>&1
:configured
mkdir C:\ProgramData\ssh > nul 2>&1
> C:\ProgramData\ssh\sshd_config echo # exynos-woa (extra-ssh-install.cmd): key-only login from the PC
>> C:\ProgramData\ssh\sshd_config echo Port 22
>> C:\ProgramData\ssh\sshd_config echo PubkeyAuthentication yes
>> C:\ProgramData\ssh\sshd_config echo PasswordAuthentication no
>> C:\ProgramData\ssh\sshd_config echo KbdInteractiveAuthentication no
>> C:\ProgramData\ssh\sshd_config echo AuthorizedKeysFile .ssh/authorized_keys
>> C:\ProgramData\ssh\sshd_config echo Subsystem sftp sftp-server.exe
>> C:\ProgramData\ssh\sshd_config echo Match Group administrators
>> C:\ProgramData\ssh\sshd_config echo        AuthorizedKeysFile __PROGRAMDATA__/ssh/administrators_authorized_keys
copy /y "%S%\authorized_keys" C:\ProgramData\ssh\administrators_authorized_keys > "%X%\key.txt" 2>&1
icacls C:\ProgramData\ssh\administrators_authorized_keys /inheritance:r /grant "*S-1-5-32-544:F" /grant "*S-1-5-18:F" >> "%X%\key.txt" 2>&1
netsh advfirewall firewall delete rule name="exynos-woa sshd" > nul 2>&1
netsh advfirewall firewall add rule name="exynos-woa sshd" dir=in action=allow protocol=TCP localport=22 remoteip=192.168.137.0/24 > "%X%\firewall.txt" 2>&1
sc config sshd start= auto > "%X%\service.txt" 2>&1
net stop sshd >> "%X%\service.txt" 2>&1
net start sshd >> "%X%\service.txt" 2>&1
sc query sshd >> "%X%\service.txt" 2>&1
net localgroup administrators > "%X%\admins.txt" 2>&1
s10ediag mark x: ssh install done
s10ediag flush > nul 2>&1
