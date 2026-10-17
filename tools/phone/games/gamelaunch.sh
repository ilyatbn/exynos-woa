#!/bin/bash
# gamelaunch.sh 'C:\Games\src\SPEED2.EXE' - run gamelaunch.ps1 in the user's session, print its log, the Application
# event log errors of the last 3 minutes, and the newest UMD trace log of the process if one appeared.
H=$(cd "$(dirname "$0")" && pwd)
echo "$1" | ssh s10e 'powershell -NoProfile -Command "$input | Set-Content C:\S10eTmp\gamelaunch.args"'
scp -q -O "$H/gamelaunch.ps1" s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\run-as-user.ps1 C:\S10eTmp\gamelaunch.ps1 40' < /dev/null > /dev/null
ssh s10e 'type C:\S10eTmp\gamelaunch.out' < /dev/null
scp -q -O "$H/appevents.ps1" s10e:C:/S10eTmp/ < /dev/null
ssh s10e 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\appevents.ps1 -Minutes 3' < /dev/null
