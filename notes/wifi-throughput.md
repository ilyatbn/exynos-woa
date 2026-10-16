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
- 23:05 1.0.9.8 on device: the firmware ALREADY had PM 2 (PM_FAST) -> the PM set was a no-op (left in, harmless, documents the mode). Ping RTT idle 4-210 avg 118 ms (PM_FAST dozes after idle = expected for 1 ping/s); DURING a 20 MiB download: 24-749 avg 258 ms = deep tx queue (~450 KB at 1.75 MB/s) -> the air tx rate is the bottleneck, not polling/power save. Lead: 802.11n without A-MPDU tops out ~15-20 Mbit/s. 1.0.9.9 logs ampdu / ampdu_ba_wsize / ampdu_mpdu / frameburst / nmode / vhtmode on association.
- 23:10 1.0.9.9: ampdu 1, ampdu_ba_wsize 64, ampdu_mpdu -1, frameburst unsupported (MSG 6), nmode 1, vhtmode 1 -> aggregation is enabled in firmware; the ~14 Mbit/s tx ceiling with a deep tx queue stays OPEN (next: tx flowring priority/TID vs BA sessions, tx completion pacing, rx ACK latency through the 5 ms poll). Transfers themselves are correct (raw ssh stream / scp -O, 20 MiB intact). Parked as follow-up; goal work continues on video + BT audio.
- 2026-10-15 23:00 user: back on 1.0.0.15 BT after a restart = "working with a choppy 2.4ghz"; asks to find why 2.4 GHz
  gives ~15 Mbit/s ("we should get 5x that at least"), theory: the missing efficiency is also what starves BT
  (shared radio, airtime). Starting a tx-path audit: tx post flags/priority, flowring setup, completion pacing.
- 23:10 tx flags audit: 802.3, prio 0, tid-0 flowring = fmac-equivalent (not the cause). Posting: 1 descriptor + doorbell per frame, 64 tx buffers; at 1.75 MB/s a 64-frame batch takes ~55 ms to complete => time spent in firmware/air. Exynos9820WlanCx 1.0.10.0: GET_VAR 'dump' (ampdu, rssi, wme) logged every ~1 min while associated.
- **2026-10-15 23:20 ROOT CAUSE FOUND (firmware 'dump ampdu', 1.0.10.0, during 5 x 20 MiB phone->PC):**
  TX: `txaddbareq 5 rxaddbaresp 0` (5 ADDBA requests sent, 0 responses), `ini 1 ini_off 1 ini_on 0`,
  `tot_mpdus 0 tot_ampdus 0` -> NO tx aggregation at all; `AQM_ENAB 1`, frameburst histogram avg 3.
  RX: `rxaddbareq 0`, `rxampdu 70027 rxmpdu 70027 rxmpduperampdu 1`, rxht 100 % MCS7 SGI -> the AP sends HT frames
  but never sets up a BA session towards us either. => no Block Ack in either direction = every frame individually
  acked = ~11g-class efficiency (~14 Mbit/s) and far more airtime per byte (starves BT on the shared radio - user's
  theory). 'dump rssi'/'dump wme' unsupported (MSG 6).
  Next suspect: the association is not QoS/WMM (BA requires a QoS association) -> check iovar 'wme' and the WMM IE
  in our join/assoc request (WDI WMMQoSEnabled path, notes/m12-wifi-wificx.md:318); then ADDBA action-frame handling.
- Battery 12 % at 23:20 - user asked to pause below 10 % to charge.
- 23:25 WMM is negotiated ('[join] association result: status 0, phy 8, wmm 1, req 164 B resp 219 B') -> QoS is not the
  blocker. Open: why our ADDBA requests (5) get no ADDBA response and the AP never initiates one (rxaddbareq 0) although
  it sends us HT MCS7. Next: decode the HT Capabilities element in our 164 B association request (A-MPDU params /
  "assoc_req_ies"), compare with what Android's dhd joins with; check whether action frames from the AP reach the
  firmware. Paused for battery (12 %). State: BT 1.0.0.15 (= 1.0.0.10 code), Wi-Fi 1.0.10.0 (dump every ~1 min).
- 23:55 load test #2 with BT 1.0.0.16 (ACL priority high while streaming): user 'its good' / 'bluetooth is still working fine' during the BBB phone->PC copy (scp -O), copy steady 1.2-1.6 MB/s (first sample inflated). Speedtest on the phone: 8 Mbit/s. Exynos9820WlanCx 1.0.10.1: [ie req]/[ie resp] dump of the association elements (ids+lens; full bytes only for HT cap 45, HT op 61, ext cap 127, VHT 191/192, WMM) to see what A-MPDU parameters are negotiated.
- 2026-10-16 00:00 1.0.10.1 association elements (no SSID/addresses kept): ours HT cap 2D 00 1B FF FF 00.. = LDPC, SMPS
  disabled, SGI20, A-MPDU exp 3 (64 KB) density 6, MCS 0-15; ext cap present; WMM info 00 50 F2 02 00 01 00.
  AP: HT cap 6F 10 17 FF FF FF.., HT op prim ch 9 (40 MHz bits set), VHT cap/op, ext cap, WMM parameter element.
  Firmware: ampdu 1, ampdu_tx 1, ampdu_rx 1, ba_wsize 64, wme -1 (auto), wme_apsd 1. => capabilities fine on both
  sides; Block Ack still never set up. Leading hypothesis: timing/authorization - firmware's ADDBA requests (5) go
  out before Windows' supplicant finishes the 4-way handshake, AP ignores them, firmware gives up (ini_off 1);
  AP-side ADDBA never comes. Next experiment: firmware "send ADDBA" iovar for TID 0 to the AP after keys are set.
- 2026-10-16 00:10 **likely root cause: the firmware is never told the port is authorized.** dhd (Samsung bcmdhd_101_16,
  LineageOS exynos9820 tree) wl_cfg80211.c:9340-9394: after the host supplicant's 4-way handshake cfg80211 calls
  change_station(AUTHORIZED) -> wldev_ioctl_set(WLC_SCB_AUTHORIZE, peer mac) in station mode. WLC_SCB_AUTHORIZE = 121
  (wlioctl_defs.h:764; fmac fwil.h:59 BRCMF_C_SET_SCB_AUTHORIZE). Our WlanCx never sent it (Windows' supplicant
  plumbs keys via WDI_SET_ADD_CIPHER_KEYS -> wsec_key only). 1.0.10.2: WlanMsgScbAuthorize(peer) right after the
  pairwise key is installed. Verify: '[key] SCB_AUTHORIZE -> MSG 0', then 'dump ampdu' tot_ampdus > 0, rxaddbaresp > 0.
- 00:20 1.0.10.2 on device: '[key] SCB_AUTHORIZE -> MSG 0 (status 0)' after the pairwise key, but dump unchanged (txaddbareq 5 rxaddbaresp 0, ini_off 1, tot_ampdus 0, rxaddbareq 0); 20 MiB raw = 1.08 / 1.25 MB/s. Authorization alone is not it. Timing hypothesis: the 5 ADDBA attempts happen during the handshake (EAPOL goes out as prio 0 on the tid-0 flowring -> TID 0 traffic), unanswered, initiator off. 1.0.10.3: after authorize, ampdu_tid {0,off} then {0,on} (wlioctl.h:6395, dhd_linux.c:12194) to restart the initiator.
- 00:30 1.0.10.3: ampdu_tid 0 off/on accepted (MSG 0) after SCB_AUTHORIZE; later dump: txaddbareq 13 (was 5) rxaddbaresp 0, ini_off 1, tot_ampdus 0 -> the reset DID make the firmware send 8 more ADDBA requests while authorized, and the AP still never answers (and never requests one itself: rxaddbareq 0). => not a timing/authorization problem on our side; either our ADDBA frames don't reach the AP, the AP refuses BA with this station, or the AP's responses are lost before the firmware's BA handler. Needs a reference: does this AP aggregate with this phone under Android / with other clients?
- 00:40 user: PC reconnected to the same Wi-Fi; 'the problem is with the driver'. PC: Intel AX210 LinkSpeed 300 Mbps on that network (AP does wide/2x2 high-rate on 2.4 GHz); internet download from the PC 9.3 Mbit/s = WAN-limited, not a Wi-Fi measure. Our phone's HT cap 2D 00 = 20 MHz only (AP HT op is 40 MHz) -> caps at 144 not 13. Both-direction BA failure suggests BA action frames are dropped both ways; candidate: PMF (802.11w) half-negotiated (BA action frames are robust mgmt frames). Next: full RSN IE (id 48) dump of req/resp.
- 00:45 user: fast.com on the PC (same Wi-Fi) > 100 Mbit/s. 1.0.10.4: full RSN IE dump of our request (id 48) and the AP's RSN from the scan entry ('[ie ap-rsn]') to check the PMF capability bits.
- 00:50 1.0.10.4 RSN: AP 01 00 | 00-0F-AC-04 | 1 x CCMP | 2 AKMs 00-0F-AC-02 PSK + 00-0F-AC-08 SAE | caps 8C 00 (MFPC, PTKSA replay counters 16); ours caps 00 00 (no MFP - PMF NOT half-negotiated, theory out - but 1 PTKSA replay counter). 1.0.10.5: our RSN caps 0x000C (16 PTKSA replay counters, 802.11-2016 9.4.2.25.4; matches the AP's own bits). Hypothesis: AP will not set up BA with a 1-replay-counter QoS client.
- 01:00 1.0.10.5 on device: request RSN caps 0C 00 confirmed; 20 MiB raw 1.39 / 2.00 MB/s; dump still ini_off 1, tot_ampdus 0, rxaddbareq 0 -> replay counters alone not the fix (speed noise).
- **2026-10-16 02:40 LAN speed test (user away a few hours: "make it go faster, 100% a device issue").** 02:09 silent
  freeze -> firmware "Windows died 2 times in a row" -> TWRP; `adb reboot` back to Windows. Test rig: Python
  ThreadingHTTPServer on the PC (scratchpad srv/speedsrv.py, TCP 9999 opened by the user; GET 100 MiB random file,
  POST /upload discards), phone side C:\S10eTmp\speedtest.cmd (curl.exe). PC loopback 6.8 Gbit/s (server not a limit).
  Baseline WlanCx 1.0.10.7, 2.4 GHz: **DOWN (PC->phone, phone RX) 100 MiB in 233 s = 3.6 Mbit/s; UP (phone TX) 50 MiB
  in 20 s = 20.8 Mbit/s.** RX is 6x worse than TX -> host RX path is the first suspect (rx buffer posting / poll).
- 02:50 WlanCx 1.0.10.8: "counters" decode (wlc rxframe/rxnobuf, macstat rxf0ovfl) on every alive tick (~6 s) +
  debug channel Parameters\DbgDump (REG_SZ, one-shot): "<dump section>" | "var:<iovar>" | (1.0.10.9) "pm:<0-2>".
  TCP download: fw rxframe == host rx fifo in == out, rxnobuf 0, rxf0ovfl 0 -> nothing lost in firmware/host.
  PC side: 1.1-1.8 % TCP retransmits; pktmon capture (srv/tcpan.py, deduped by IP id): phone adv window median 128 KB,
  but in flight median 16 KB, dup-ACK runs up to 30 + SACK, 60 send gaps > 50 ms (RTO-like 0.3-0.6 s) in 15 s.
- 03:05 **UDP PC->phone, numbered 1400 B (srv/udptx.py -> C:\S10eTmp\udprx.ps1; phone firewall rule S10eUdpTest
  UDP 9998 added):** offered 10 Mbit/s -> 18-21 % lost (953 twice) + 500-670 out-of-order; 30 -> 8.4 Mbit/s received
  (68 % lost); 60 -> 7.5. Firmware rxframe delta == host fifo delta == what the app got -> **frames never reach the
  phone's wlc layer: lost/reordered between the AP and our radio** (air or AP queueing), the phone rx caps ~750 fr/s.
  PM 0 (CAM, via DbgDump pm:0 on 1.0.10.9; was 2): unchanged (10 -> 84 % lost, 30 -> 8.4, 60 -> 7.5) -> not power save.
  1.0.10.10: macstat80 line (txall/txack/txucast/rsptmout, rxstrt/badfcs/badplcp/crsglitch/anyerr/toolate/dtocast/bcn/
  missbcn; wl_cnt_ge80mcst_v1_t from LineageOS exynos9820 bcmdhd_101_16 wlioctl.h, uint32 index x 4, cross-checked:
  rxdtucastmbss 76 / rxf0ovfl 256 = the already-verified offsets) to see whether the radio fails to receive.
- 03:20 silent freeze #2 tonight (ssh closed, ping half-answering ~1 min, then reboot; uptime 1.2 min at 03:22;
  stay.flg brought it back to Windows). 02:09 was the same signature. WlanCx 1.0.10.12 persisted across the reset.
- 03:15 **RX cap explained (1.0.10.12 macstat80 deltas during UDP 30 Mbit/s):** rxrts +1598, txcts +1573, txack +1703,
  rxtoolate +1441 -> the AP sends an RTS before EVERY data frame to us (we never send RTS: txrts 0). Without a BA
  session every MPDU is its own TXOP: RTS + CTS (legacy rate in 2.4 GHz, ~0.7 ms at 1 Mbit/s DSSS) + data + ACK +
  backoff ~ 1.2 ms/frame -> ~800 fr/s x 1400 B ~ 9-10 Mbit/s = exactly the measured cap. rxtoolate tracks the RTS
  count (likely a side counter, not the loss itself). AP HT op byte 2 = 0x06: HT protection mode 2 (20 MHz
  protection, triggered by a 20 MHz-only HT STA = us) + non-greenfield present. => the BA failure is THE cause;
  aggregation would amortise the RTS/CTS over up to 64 MPDUs.
  Ruled out on the way: power save (PM 0 no change), BT coex (BT device disabled: reordering 500-800 -> 0 but cap
  unchanged; btc_mode 1 -> 0 no change), chains (rsdb_mode 0, txchain/rxchain 3, tx/rxstreams 2, hw chains 3),
  host rx path (fw rxframe == fifo in == delivered). mimo_bw_cap 0 (20 MHz in 2.4 GHz).
- 03:25 **5 GHz A/B (HomeX-5, ch 36, via a SYSTEM schtask with automatic fallback to the 2.4 profile):** UDP 30/60
  Mbit/s 0 % loss, TCP 100 MiB down 20.9 MB/s = **167 Mbit/s**; dump ampdu: rxaddbareq 2 (AP initiates), txaddbareq 1
  rxaddbaresp 1, tot_ampdus 1585 (8 MPDU/A-MPDU tx, 6 rx). => host driver/data path is fine; the failure is 2.4 GHz only.
  dump rsdb: 5 GHz STA on wlc0, 2.4 GHz STA on wlc1; rsdb_mode = {config 0, status 1} (wl_config_t).
- 03:30 2.4 GHz again with btc_mode 0 set BEFORE a fresh association (disconnect + connect, BT device still
  disabled): still txaddbareq 11 rxaddbaresp 0, rxaddbareq 0, tot_ampdus 0 -> not BT coex.
- sta_info for the AP (1.0.10.13 "stainfo"): ver 4 len 84 only (stats beyond 84 not returned), flags 0x0011803B =
  BRCM | WME | AUTHE | ASSOC | AUTHO | AMPDU_CAP | AMSDU_CAP | VHT_CAP (wlioctl_defs.h:121-148) -> firmware treats the
  AP as aggregation-capable.
- Samsung dhd differences checked (bcmdhd_101_16): Makefile:869 SUPPORT_2G_VHT/SUPPORT_5G_1024QAM_VHT ->
  vht_features |= 7 before UP. Tried in 1.0.10.15: firmware default 0x6, set 0x7 -> BCME_BADOPTION (-3) - dhd treats
  that code as expected ("need to check BANDLOCK", dhd_linux.c:11878-11883) => Android runs without 2.4 VHT too;
  removed again. bw_cap: dhd only reads it (wl_cfg80211.c:16041) -> Android is 20 MHz on 2.4 too. rsdb_mode: dhd sets
  it only in MFG mode / antenna-test file (dhd_linux.c:10343, dhd_custom_sec.c:411-461) -> same default as ours.
- 03:42 **2.4 vs 5 GHz association elements (1.0.10.17 full vendor IEs; no addresses kept):** they are DIFFERENT
  APs. HomeX-5 (ch 36) = HE/Wi-Fi 6 router, vendor OUI 8C:FD:F0 (Qualcomm), WPS IE. Home-X (ch 9, rssi -39, 20/40 MHz
  chanspec 0x1907) = a separate older box: BRCM IE 00:10:18, Broadcom 2.4 GHz-VHT vendor IE 00:90:4C:04 (its VHT MCS map
  0xFFFF = no VHT streams), and a 00:0C:42 (MikroTik/Routerboard) identity IE carrying a device name (not recorded).
  Our 2.4 request already carries the 00:90:4C:04 2.4 GHz-VHT vendor IE (VHT cap 32 78 81 0F, MCS map FAFF) and BRCM IE.
  Scan: no 2.4 GHz BSS of the Wi-Fi 6 router visible; ch 9 overlaps "Yura-EXT" ch 11 at -55 and 6 weaker BSSs.
  The user's PC ">100 Mbit/s" most likely went through the 5 GHz router. **Conclusion so far: on Home-X 2.4 the AP never
  initiates BA, never answers our ADDBA (13/13) and RTS-protects every MPDU -> ~10 Mbit/s cap; our host path does 167
  Mbit/s with BA on the other AP.** Open: does that AP aggregate with ANY 2.4 client? (test: the PC on Home-X 2.4;
  or check the AP's A-MPDU / WMM / hw-protection settings.) Monitor mode to sniff it: WLC_SET_MONITOR -> BCME_UNSUPPORTED
  (-23) on this fw (code removed). BT device re-enabled; phone firewall rule S10eUdpTest (UDP 9998) still present.
- Observation: the first UDP/TCP test right after every WlanCx reload is 1.6-3.4 Mbit/s, later ones ~8-10 (2.4 GHz).
- Commit (1.0.10.19, diagnostics only): "counters" decode + macstat80 lines (every ~1 min while associated, + "cnt"),
  debug channel Parameters\DbgDump (section | var: | set: | pm: | stainfo | cnt), sta_info decode (only the fields the
  reply's len covers), full IE bytes for rates/ext-rates/op-classes/vendor IEs. Verified on device: var:rsdb_mode ->
  {0, 1}, cnt -> wlc + macstat80 lines, stainfo -> flags line only (len 84), set:btc_mode=1 -> MSG 0.
- Review of feb9c86 (adversarial): all 23 MCST80_* offsets re-derived by script from wl_cnt_ge80mcst_v1_t (uint32
  index x 4) - all match (MISSBCN = missbcn_dbg); every decoded offset < 260 = the xtlv length guard; sta_info reads
  stay inside RespLen / the struct len; GetVarRaw reads 16 B only when RespLen >= 16; no address / IP / device name
  in the diff (grep). FOUND: (1) "set:x=" (no digits) silently wrote 0, (2) a section name >= 32 chars was skipped
  without a log line. FIXED in 1.0.10.20 (follow-up commit): both rejected with a "[dbg]" line; device-verified
  (set:btc_mode= / =1x ignored, =1 MSG 0, 34-char name rejected).
