# Wi-Fi throughput (BCM4375, Exynos9820WlanCx)

User report 2026-10-15: ~18 Mbit/s while copying a 725 MB file to the phone over scp. No addresses in this file.

## Progress log
- 2026-10-15 20:55 snapshot during the scp (netsh wlan show interfaces + typeperf 3 x 2 s):
  - FACT: connected on 2.4 GHz, channel 9, signal 90 %, "Radio type 802.11ac", WPA2-PSK CCMP.
  - FACT: "Receive/Transmit rate 54 Mbps" is NOT measured - it is our placeholder:
    drivers/Exynos9820WlanCx/CxAdapter.cpp:13 `#define CX_LINK_SPEED 54000000ull /* placeholder link speed ... until
    W4 reports the real rate */` (+ CxConnect.cpp:16). The real PHY rate is unknown.
  - FACT: Wi-Fi Bytes Received/sec 1.21 / 1.74 / 2.09 MB/s (= 10-17 Mbit/s payload).
  - FACT: CPU not the limit: total 22 / 15 / 8 %, one core peaked 58 % (likely sshd), DPC time <= 1.6 %,
    interrupt time <= 0.8 % on every core.
  - INFERENCE: ~17 Mbit/s of TCP payload is what a 54 Mbit/s 802.11g link delivers; if the firmware associated
    without HT (11n) on this 2.4 GHz AP, that alone explains it. Alternatives: a crowded 2.4 GHz channel, or a
    per-packet latency limit in our D2H/H2D data path (not CPU-bound).
  - Next (cheapest first): read the firmware's real link rate / PHY mode (bcmdhd "rate"/"nrate" iovar or the
    associated BSS's HT/VHT IEs) and report it instead of the placeholder; try the 5 GHz network of the same AP;
    measure with an unencrypted TCP stream to separate scp/ssh cost.
- 2026-10-15 22:30 "downloads stop at 204800 B" is NOT Wi-Fi: same 1 MiB file phone->PC: `ssh 'type file' > f` =
  1048576 B (2.7 s incl. setup), `scp -O` (legacy protocol) = 1048576 B, `scp` default (SFTP mode, Git-for-Windows
  OpenSSH client vs the phone's Windows OpenSSH sftp-server) = 204800 B with exit 0. => use `scp -O` (or a raw ssh
  stream) for phone->PC copies. Root cause in the SFTP pairing not investigated (not ours).
- 2026-10-15 22:45 raw 20 MiB phone->PC = 12.0 s (1.75 MB/s, ~14 Mbit/s). Exynos9820WlanCx 1.0.9.7: WlanMsgGetRate (WLC_GET_RATE 12, fmac fwil.h:18, 500 kbit/s units) on link-up and every alive tick (60 s, worker thread = the only command path); logged '[link] firmware rate' and reported to NetAdapterCx instead of the 54 Mbit/s placeholder.
- 22:55 1.0.9.7 on device: '[link] firmware rate 144.0 Mbit/s' (HT 2x2 MCS15 20 MHz) - the radio is fine; netsh still shows 54 (WDI LINK_STATE_CHANGE keeps CX_LINK_KBPS, cosmetic). Ping RTT PC->phone 20x: min 8 / avg 137 / max 207 ms = power-save beacon buffering; the driver never set PM. 1.0.9.8: on link-up GET_PM (logged) + SET_PM PM_FAST (fwil.h:51-52 85/86, defs.h:76-78 PM_FAST 2, = fmac default cfg80211.c:7824).
