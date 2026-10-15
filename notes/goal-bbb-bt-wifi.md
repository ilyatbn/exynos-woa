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
