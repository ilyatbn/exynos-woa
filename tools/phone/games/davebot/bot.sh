#!/bin/bash
# bot.sh CMD [ARGS] - PC side of davebot.ps1 (notes/games-unity.md):
#   bot.sh start              push davebot + keyplay, start the hidden bot in the user session
#   bot.sh shot NAME          screenshot -> traces/games/bot/NAME.png
#   bot.sh key SCAN EXT MS    one key press (scan code hex, extended flag, hold ms)
#   bot.sh tap X Y            one touch tap at physical pixels
#   bot.sh log                tail of bot.log
#   bot.sh quit
R=/c/Users/ilya/code/exynos-woa; OUT=$R/traces/games/bot; mkdir -p "$OUT"
req() { ssh s10e "echo $2> C:\\S10eTmp\\bot\\$1.req" < /dev/null; }
waitgone() { for i in $(seq 1 ${2:-60}); do ssh s10e "if exist C:\\S10eTmp\\bot\\$1.req (exit 1)" < /dev/null && return 0; sleep 0.5; done; echo "timeout $1"; return 1; }
case "$1" in
  start)
    scp -O -q "$R"/tools/phone/games/davebot/davebot.ps1 "$R"/tools/phone/games/davebot/davebot-start.ps1 \
      "$R"/tools/phone/games/keyrec/keyplay.exe s10e:C:/S10eTmp/ || exit 1
    ssh s10e 'mkdir C:\S10eTmp\bot 2>nul & powershell -NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\userrun.ps1 -Exe powershell.exe -Args "-NoProfile -ExecutionPolicy Bypass -File C:\S10eTmp\davebot-start.ps1" -Wait 15' < /dev/null
    sleep 3; ssh s10e 'type C:\S10eTmp\bot\bot.log' < /dev/null | tail -2 ;;
  shot) req shot x; waitgone shot && scp -O -q s10e:C:/S10eTmp/bot/shot.png "$OUT/$2.png" && echo "$OUT/$2.png" ;;
  key) req key "$2 $3 $4"; waitgone key ;;
  tap) req tap "$2 $3"; waitgone tap ;;
  launch) req launch x; waitgone launch ;;
  log) ssh s10e 'type C:\S10eTmp\bot\bot.log' < /dev/null | tail -${2:-10} ;;
  quit) req quit x ;;
esac
