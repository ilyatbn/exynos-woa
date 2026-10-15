# Goal (user, 2026-10-15 21:05): Big Buck Bunny in mpv with proper render + audio on Bluetooth headphones + working
# Wi-Fi transfers

Verbatim: "your goal is to properly play big buck bunny with proper render through mpv, while also streaming to my
bluethooth headphones, and properly transfer what you need thorugh wifi". No addresses in this file.

## State at start (all FACT, this session)
- Video: C:\S10eTmp\videos\bbb1080.mov (Blender, CC-BY; H.264 Main 1920x1080 24 fps 9.3 Mbit/s, AAC 5.1 48 kHz,
  9:56). h264_exynos decodes 60 s at 78 fps / 3.25x realtime; 33 of 1440 frames carry an MFC warning flag
  (FF_DECODE_ERROR_CONCEALMENT_ACTIVE -> "corrupt decoded frame"); bit-exactness vs software NOT yet measured.
- Render: mpv vo=gpu/d3d11 on the Mali UMD = BLACK window (scan-out dump fb-default/fb-blt: desktop + taskbar fine,
  mpv window solid black) in both flip-model and --d3d11-flip=no. --d3d11-warp=yes (Microsoft WARP) = correct
  picture (fb-warp: Big Buck Bunny frame, 99.9 % non-black), MFC decode, no video drops. => bug in maliumd.dll
  (Mesa d3d10umd) rendering mpv's shaders/formats, not decode/present/DWM.
- Bluetooth: user reports BT "probably crashed" after connecting headphones. UART HCI link at 115200 baud
  (~10.5 KiB/s measured) - A2DP SBC needs ~40 KB/s (~328 kbit/s) -> the link cannot carry it. User: "40kbps is
  also way too low. get sbc/aac codecs" (A2DP's mandatory codec is SBC; Windows' A2DP source does the encoding -
  the transport is what must get faster).
- Wi-Fi: phone->PC downloads stop silently at exactly 204800 bytes (scp of 9.9 MB and 570 KB files, twice);
  PC->phone uploads fine (725 MB, 1.7 MB/s avg). Link rate unknown (54 Mbps = placeholder, notes/wifi-throughput.md).

## Plan (one commit per task)
- T1 video now: mpv.conf d3d11-warp=yes as interim (user can watch); measure CPU/drops for the whole film.
- T2 Wi-Fi TX stall at 200 KiB (blocks getting dumps/logs off the phone) - KD trace of the TX path.
- T3 Bluetooth: state after the headphone connect (driver/device status, BtUart counters), then raise the UART
  baud (BCM vendor HCI Update_UART_Baud_Rate + host UART divisor; constants from sources, never guessed), then
  A2DP to headphones.
- T4 Mali UMD black output for mpv (find the failing format/shader path; compare with d3dtri which renders).
- T5 Wi-Fi rate (real PHY rate, HT/VHT, 5 GHz).
- T6 the full run: BBB 1080p in mpv on Mali, audio on BT headphones, verified by scan-out dumps + user.

## Progress log
- 21:12 T1: mpv.conf d3d11-warp=yes deployed (portable_config); fb-warp dump showed the correct BBB frame.
- 21:20 T3 state: headphones (WH-1000XM4) paired, then the controller stopped answering: System log BthMini 3 "A
  command sent to the adapter has timed out" every ~30 s from 12:09 (phone clock) on; radio device still "OK".
  Cause (INFERENCE from notes/bt-driver.md RFCOMM test): 115200 = 10.5 KiB/s ceiling + per-byte TX busy-wait at
  DISPATCH starving the RX DPC under sustained A2DP; the H4 stream lost sync.
- 21:30 Exynos9820Bt 1.0.0.4 built: (1) 1 Mbaud after bring-up: Broadcom 0xFC18 {le16 0, le32 1000000} verified
  against Linux btbcm.h:12-15 (kernel.org) + hci_bcm.c:178-224/600-625 order; host UBRDIV 0 / UFRACVAL 10 from the
  26 MHz clock (same formula reproduces the shipped 115200 pair 13/2); verified by READ_LOCAL_VERSION at the new
  rate, fallback to 115200; bring-up retries HCI_RESET at 1 Mbaud (driver restart within a boot). (2) interrupt-
  driven TX: 16 KiB TX ring filled by BtUartTxQueue, FIFO refilled from the ISR on the TXD level interrupt, one
  mask helper (BtUartSetMask) for RX-throttle/TX state; bounded 2 s wait when the ring is full (counted).
- 21:40 1.0.0.4 on device (clean boot): HCI_RESET 00, patchram 1261 records 0 failed, reset after patch 00; 0xFC18 -> status 00, then READ_LOCAL_VERSION at 1 Mbaud = no answer AND none at 115200 => the controller switched; verify came 100 us after the divisor change. 1.0.0.5: 20 ms settle + RX FIFO reset + 3 tries; installing it now also tests the divisor directly (reset at 115200 must fail, the retry at 1 Mbaud must answer).
- 21:50 1.0.0.5 install (controller left at the 0xFC18 rate): HCI_RESET ff at 115200 AND ff at UBRDIV 0/UFRACVAL 10 (PnP retries, same). Linux samsung_tty.c:1398-1416 + 1510-1514 computes the same 0/10 for 1 Mbaud at 26 MHz (UBRDIV 0 allowed). Gap: 115200's 13/2 never proved UFRACVAL works (13/0 = 116071, +0.75 %); if ignored, 0/x = 1.625 Mbaud. 1.0.0.6: on failure scan UBRDIV 0-1 x UFRACVAL 0-15 (HCI_RESET, 150 ms) and log the answering pair.
- 22:00 1.0.0.6: scan UBRDIV 0-1 x UFRACVAL 0-15 -> no answer anywhere (controller at a rate outside 0.81-1.63 Mbaud, or hung / CTS held). Stop guessing: 1.0.0.7 measures the host rate itself (loopback UCON bit 5 serial_s3c.h:86, AFC off, 64 B, QPC to UTRSTAT TXE) at 13/2 and 0/10, logs both, and only sends 0xFC18 when 0/10 measures 1 Mbaud +-3 %. Install + reboot (controller needs the power cycle).
- 22:05 1.0.0.7 measurement (pre-reboot install): 13/2 -> 114985 baud (fraction applies), 0/10 -> 1584761 (fraction ignored at UBRDIV 0: 26 MHz/16 = 1625000, window reads ~2.5 % low) => explains 1.0.0.4-6: controller at 1 Mbaud, host at 1.6. 1.0.0.8: operating speed 1625000 = UBRDIV 0/UFRACVAL 0 exactly (only exact rate >= 1 Mbaud from 26 MHz); gate +-4 %.
- 22:15 After the reboot 1.0.0.7 failed HCI_RESET at 115200 although the DXE power-cycles bt_en every boot (Beyond0LteBtUartDxe.c:332-345, low 20 ms / high / 150 ms): the loopback test bytes also reach the controller's RX (at 1.6 Mbaud = garbage) and knock its H4 parser out of sync before the first reset; every PnP retry repeats it. Measurement consistent across 5 runs (13/2 -> 114960-114997, 0/10 -> 1577550-1584761). 1.0.0.9: measurement + blind scan removed from bring-up; straight to 1625000 with verify/fallback.
- 22:20 user: once BT works, download free music + foobar2000 (or another player) and play a song over BT (added as T7).
- 22:25 **1.0.0.9 clean boot: HCI_RESET 00, patchram 1261/0 failed, reset after patch 00, 0xFC18 1625000 -> READ_LOCAL_VERSION subver 1111 at 1625000 after 1 try.** Link now 1.625 Mbaud (~158 KiB/s, 16x).
- 22:30 T2 closed: the 200 KiB 'Wi-Fi stall' is scp's SFTP mode (raw ssh stream and scp -O move 1 MiB intact) -> notes/wifi-throughput.md. Download rule: scp -O.
- 22:40 KD freeze: copilotapp.exe loading test-signed maliumd.dll -> CI first-chance break (80000003) into the attached kd; kdrun's default cmds ended with 'q' -> phone halted (user: black screen). Resumed with kdrun --cmds g; kdrun default now ends with g.
- 23:10 T5 Wi-Fi: real PHY rate 144 Mbit/s (11n 2x2), firmware already PM_FAST, A-MPDU on; throughput ~1.75 MB/s, deep tx queue; parked (notes/wifi-throughput.md). Transfers verified intact.
