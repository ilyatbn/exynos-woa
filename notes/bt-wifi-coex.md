# BT / 2.4 GHz Wi-Fi coexistence (BCM4375)

User (2026-10-15): "i need to make bt and 2.4ghz work". Facts so far (notes/goal-bbb-bt-wifi.md): A2DP to WH-1000XM4
cuts out while Wi-Fi is associated on 2.4 GHz (ch 9), worse under Wi-Fi load; clean with Wi-Fi off and on 5 GHz.
UART HCI link clean (no line errors/throttles). => radio time-sharing on the combo chip. No addresses/SSIDs here.

## Questions
- C1 Which coex settings does the NVRAM file we load carry (btc_*, boardflags BTCOEX bits), vs. what Android uses?
- C2 Which coex iovars does Samsung/Broadcom dhd set at init or at runtime (btc_mode, btc_params, btc_flags)?
- C3 Does the BT side need vendor HCI commands for coex (A2DP priority / "SCO/A2DP" hints)?
- C4 What does our WlanCx set today (nothing, per grep) and what does firmware report (GET btc_mode etc.)?

## Progress log
- C1 NVRAM (s10e-artifacts/wifi-m2/bcm4375, both module variants): btc_mode=1, btc_prisel_ant_mask=0x2, clb_swctrl_smask_ant0/1=0x3ff, aa2g=3, antswitch=0, boardflags 0x10401001 / 0x00800000 / 0x40000100.
- C1 our driver loads nvram.txt_CS01_semco_b1 (OTP module vid 0x5133, WlanCx.txt) -> firmware gets btc_mode=1; WLAN-side
  coex is configured.
- C3 (FACT, AOSP packages/modules/Bluetooth main): hcidefs.h:873-878 HCI_BRCM_SET_ACL_PRIORITY = 0xFC57, params 3 B
  {le16 handle, u8 priority}, NORMAL 0x00 / HIGH 0xFF / HIGH_LOW_LATENCY 0xF3; l2c_utils.cc:2291-2316 sends it when L2CAP
  priority is set high (A2DP streaming). Windows' Microsoft stack never sends this Broadcom vendor command => our A2DP
  link runs at normal priority in the coex arbiter. Plan: the transport injects 0xFC57 {handle, 0xFF} after an ACL
  Connection Complete and swallows its Command Complete; must respect HCI command flow control.
- 22:18 Exynos9820Bt 1.0.0.11: ACL priority injection (Driver.h CoexLock/HostCmdOutstanding/CoexInFlight/CoexPendingHandle/CoexDeferred; Io.c CoexOnEvent in ReadH4PacketComplete, CoexHostCommand from FdoWriteDeviceIO). Review note: if the controller never answers 0xFC57, the held BthPort command stays held (CoexInFlight stuck) - Broadcom answers unsupported vendor commands with an error Command Complete, so accepted; revisit if seen.
- 2026-10-15 ~22:20 1.0.0.11 on device (Wi-Fi on 2.4 GHz ch 9): driver restart path verified (HCI_RESET ff at 115200,
  00 at 1625000); KD: "coex: Set ACL Priority high -> status 00" x3 (one per ACL connection, ok 3 failed 0) - the
  BCM4375 accepts 0xFC57. Stats: 0 line errors, 0 throttles, 0 TX ring waits. (1.0.0.10's stats before: 169 KB TX,
  peak ring 868 B, 438 TXD irqs - TX is not a bottleneck.) KD detached for the listening test.
- 22:25 user: with 1.0.0.11 'its connecting to bluetooth but now windows doesnt output it to the headphones': BT devices OK, both headphone AudioEndpoints unplugged/not present (A2DP never comes up), 3 priority sets = 3 connection attempts. Own review: 1.0.0.11 held ONE BthPort command; a second one arriving meanwhile (controller credit > 1) went out past it = reordered HCI commands. 1.0.0.12: ordered FIFO (4) of held commands, 'held max' in the log, Parameters\CoexAclPriority (default 1, 0 = off) to rule the injection in/out without a rebuild.
- 22:27 1.0.0.12 (injection on, ordered FIFO): headphones "not connecting at all". Parameters\CoexAclPriority=0 +
  pnputil /restart-device -> user: "okay that worked". => the 0xFC57 injection (as implemented: at ACL Connection
  Complete, flow-controlled by HostCmdOutstanding, reply swallowed) breaks connection setup. Injection left OFF.
  Note AOSP raises priority when the A2DP stream starts (l2c_utils.cc), not at Connection Complete.
  Next: compact HCI trace (command opcodes, CC/CS opcode+credits+status, connection events, capped) with injection on
  for one connect attempt, then fix from data.
- 22:32 user: even with CoexAclPriority=0 the headphones connect but are not an output device -> the injection is NOT proven to be the cause; duplicate 'Headphones (2- ...)' endpoints suggest stale state after many driver reinstalls/hot restarts in one boot. Rebooted (injection off). 1.0.0.13 built (capped HCI trace: hci> cmd / INJECT, hci< CC/CS op num st / ConnComplete / events, budget 400), not installed yet.
