@echo off
mountvol > C:\S10eTmp\espget.log 2>&1
dir S:\ >> C:\S10eTmp\espget.log 2>&1
copy /y S:\WINPRAM.BIN C:\S10eTmp\WINPRAM.BIN >> C:\S10eTmp\espget.log 2>&1
