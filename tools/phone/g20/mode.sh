#!/bin/bash
# mode.sh list | WxH [save] | native [save] - G20 (notes/g20-scaled-modes.md): run s10emode.exe in the logged-on
# user's session (the ssh session sees only session 0's virtual display) and print its output.
# Needs C:\S10eTmp\{s10emode.exe, run-as-user.ps1}; copies modeuser.ps1 itself.
H=$(cd "$(dirname "$0")" && pwd)
echo "$*" | ssh s10e 'powershell -NoProfile -Command "$input | Set-Content C:\S10eTmp\s10emode.args"'
scp -q -O "$H/modeuser.ps1" s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'del C:\S10eTmp\s10emode.out 2>nul & powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\run-as-user.ps1 C:\S10eTmp\modeuser.ps1 30' < /dev/null > /dev/null
ssh s10e 'type C:\S10eTmp\s10emode.out' < /dev/null
