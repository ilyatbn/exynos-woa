@echo off
rem S10eAgent extra step: WiFi state snapshot first (seconds, read-only), then the gfxperf measurement (~3.5 min).
rem Deployed as \S10EDIAG\extra.cmd for interactive test boots (stay.flg).
call "%ESP%\S10EDIAG\extra-wifi-state.cmd"
call "%ESP%\S10EDIAG\extra-gfxperf.cmd"
