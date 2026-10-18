# G18 H2 - max77705 DP alt-mode entry: vendor kernel sequence (research)

Task: exact register/opcode-level H2 sequence as the Samsung Exynos 9820 vendor kernel does it.
Source tree: LineageOS/android_kernel_samsung_exynos9820, branch lineage-23.2 (same as d6 notes).
Every fact cites file:line in that tree; INFERRED marks inferences.

## Progress log
- 2026-10-18 start; read g18-hdmi-dp-alt.md, d6 P2/P3b.
- Fetched (curl, raw.githubusercontent.com, branch lineage-23.2) into scratchpad; URLs in "Sources" below.
  NOTE: include/linux/ccic/max77705.h and include/linux/mfd/max77705.h share a basename - cite as
  ccic/max77705.h vs mfd/max77705.h.

## A. Register map (ccic/max77705.h)
- 0x00 UIC_HW_REV, 0x01 UIC_FW_REV, 0x02 UIC_INT, 0x03 CC_INT, 0x04 PD_INT, 0x05 VDM_INT (ccic/max77705.h:61-67)
- 0x06 USBC_STATUS1, 0x07 USBC_STATUS2, 0x08 BC_STATUS, 0x09 FW_MINOR, 0x0A CC_STATUS0, 0x0B CC_STATUS1,
  0x0C PD_STATUS0, 0x0D PD_STATUS1 (:69-80)
- masks 0x0E UIC_INT_M, 0x0F CC_INT_M, 0x10 PD_INT_M, 0x11 VDM_INT_M (:82-85); init values UIC 0x04, CC 0x20,
  PD 0x00, VDM 0xF0 (:96-99) (1 = masked, INFERRED from usage - verify in usbc.c init).
- 0x21 REG_OPCODE, 0x22 REG_OPCODE_DATA, 0x51 REG_OPCDE_RES (sic), 0x41 REG_END_DATA (:87-91).
- UIC_INT bits: b7 APCmdResI (opcode response ready), b6 SYSMsgI, b5 VBUSDetI, b4 VbADCI, b3 DCDTmoI,
  b1 CHGTypI, b0 UIDADCI (:104-110).
- CC_INT bits: b7 VCONNOCPI, b6 VSAFE0VI, b5 AttachSrcErrI, b4 VCONNSCI, b3 CCPinStatI, b2 CCIStatI,
  b1 CCVcnStatI, b0 CCStatI (:115-122).
- PD_INT bits: b7 PDMsgI, b5 DataRole, b1 SSAccI, b0 FCTIDI (:127-130).
- Header labels are shifted: bits labelled "REG_CC_STATUS1" (CCPinStat 7:6, CCIStat 5:4, CCVcnStat b3,
  CCStat 2:0) belong to 0x0A CC_STATUS0; "REG_CC_STATUS2" (CCSBUSHORT 7:6, VCONNOCP b5, VCONNSC b4, VSAFE0V b3,
  AttachSrcErr b2, ConnStat b1, **Altmode b0**) belong to 0x0B CC_STATUS1 (:150-166). (consistent with H1 decode)
- PD_STATUS1: b7 PD_DataRole (1=DFP), b4 PSRDY, 3:0 FCT_ID (:176-178).
- VDM_INT (0x05) bits = enum VDM_MSG_IRQ_State (alternate.h:364-373): b0 DISCOVER_ID, b1 DISCOVER_SVIDS,
  b2 DISCOVER_MODES, b3 ENTER_MODE, b4 EXIT_MODE, b5 ATTENTION, b6 DP_STATUS_UPDATE, b7 DP_CONFIGURE.
  WRONG - superseded by section D: these are software flags; register bits differ. VDM_INT_M init 0xF0 would leave b0-b3 unmasked
  and mask b4-b7 (INFERRED - check whether init is overridden).

## B. Opcode protocol constants (ccic/max77705.h:503-518)
- OPCODE_WAIT_TIMEOUT 3000 ms; OPCODE_WRITE 0x21; OPCODE_WRITE_END 0x41; OPCODE_READ 0x51; OPCODE_DATA_LENGTH 32;
  OPCODE_MAX_LENGTH 33 (opcode byte + 32 data).
- Opcodes (enum :520-586): CCCTRL1_R/W 0x0B/0x0C, ... OPCODE_READ_SBU 0x25, SWAP_REQUEST 0x37,
  VDM_DISCOVER_IDENETITY_RESPONSE 0x40, **VDM_DISCOVER_SET_VDM_REQ 0x48**, VDM_DISCOVER_GET_VDM_RESP 0x4B,
  SEND_VDM_AFC3P0 0x4C, GET_PD_MESSAGE 0x4D, ACC_CMD_RECEIVED 0x4E, ACC_CMD_RESPOND 0x4F, SECURE_KEY_REV 0x50,
  FACTORY_TEST 0x54, **SET_ALTERNATEMODE 0x55**, FW_AUTOIBUS 0x57, READ_SELFTEST 0x59, GPIO5_CONTROL 0x5B,
  READ_MESSAGE 0x5D, GRL/FW_OPCODE_CLEAR 0x70.
- VDM command IDs used in SET_VDM_REQ (:588-596): DISCOVER_IDENTITY 0x1, SVIDS 0x2, MODES 0x3, ENTER_MODE 0x4,
  EXIT_MODE 0x5, ATTENTION 0x6, DP_STATUS 0x10, DP_CONFIGURE 0x11.
- Alternate enable values (alternate.h:534-537): ENABLE_ALTERNATE_SRCCAP 0x1, ENABLE_ALTERNATE_VDM 0x2,
  ENABLE_ALTERNATE_SRC_VDM 0x3, DISABLE 0x0.
- VDM_MSG_IRQ_STATUS (32-bit, alternate.h:539-555): b1 DiscoverID, b2 SVIDs, b3 MODEs, b4 EnterMode, b5 ExitMode,
  b6 Attention, b16 DP_Status_Update, b17 DP_Configure.
- SET_VDM_REQ payload structs (alternate.h:21-31, 557-579): byte0 SEND_VDM_BYTE_DATA {Num_Of_VDO:3 (b2:0),
  Cmd_Type:2 (b4:3), rsvd:3}; then 32-bit VDM header {VDM_command:5, rsvd:1, cmd_type:2, Object_Position:3,
  rsvd:2, Structured_VDM_Version:2, VDM_Type:1, SVID:16} (:55-62); then VDOs (DP status VDO :299-315; DP configure
  VDO :317-344: SEL_Configuration 1:0 (1 = UFP_U_as_DFP_D), signalling b2 DP1.3, b3 USB Gen2, pin assign bits
  b8..b13 A..F as DFP_D / b16..b21 as UFP_D).
- DP pin assignment masks A=0x01 B=0x02 C=0x04 D=0x08 E=0x10 F=0x20 (alternate.h:437-443); DP SVID 0xFF01
  (:222), Samsung 0x04E8 (:225).

## C. Who drives the VDM sequence (drivers/ccic/max77705_alternate.c)
- AP-sent requests all go through OPCODE_VDM_DISCOVER_SET_VDM_REQ 0x48 with prebuilt payloads:
  - DP_DISCOVER_IDENTITY (alternate.c:45-63): byte0 Num_Of_VDO=1, Cmd_Type=ACK(1) -> 0x09; VDM hdr cmd=1 REQ,
    objpos 0, SVDM v1.0, structured, SVID 0xFF00 -> 0xFF008001 (LE bytes 01 80 00 FF). Sent by
    max77705_vdm_process_set_identity_req (:277-292) via opcode_write.
  - DP_DISCOVER_ENTER_MODE (:64-80): 0x09; hdr cmd=4 REQ objpos 1 SVID 0xFF01 -> 0xFF018104 (04 81 01 FF).
    Sent by max77705_vdm_process_set_DP_enter_mode_req (:294-308) via opcode_push (queued).
  - DP_CONFIGURE (:82-120): byte0 Num_Of_VDO=2, ACK -> 0x0A; hdr cmd=17 (0x11) REQ objpos 1 SVID 0xFF01 ->
    0xFF018111 (11 81 01 FF); VDO: SEL_Configuration=2 (num_Cfg_UFP_U_as_UFP_D), Select_DP_V1p3=1, pin D bit
    (b11) -> 0x00000806 (06 08 00 00). max77705_vdm_process_set_DP_configure_mode_req(W_DATA) (:310-325)
    overwrites write_data[6] (= VDO bits 15:8, the pin-assignment byte) with W_DATA = one-hot pin mask
    (A 0x01 .. F 0x20, DP_Pin_Assignment_Data :181-189), queued via opcode_push.
    (Byte packing = GCC LE bitfield order, LSB first - INFERRED from the struct layout, standard for ARM64 GCC.)
  - read_length for these = 1+1+4+4*Num_Of_VDO (:290, :306, :323).
- The AP NEVER sends Discover Identity/SVIDs/Modes for DP: on a VDM IRQ it only READS the firmware's received
  responses with OPCODE_VDM_DISCOVER_GET_VDM_RESP 0x4B, write_data[0] = VDM id, write_length 1, read_length:
  DISCOVER_IDENTITY 31, SVIDS 31, MODES 11, ENTER_MODE 7, ATTENTION 11, DP_STATUS 11, DP_CONFIGURE 11
  (:765-868). => The max77705 FW, as DFP, autonomously runs Discover Identity -> Discover SVIDs -> Discover Modes
  (INFERRED from the AP never issuing those for the normal path; the only AP-sent Discover Identity is the
  "DEX HUB" re-kick when phone is SNK+DFP, :1123-1126 / :1160-1164).
- max77705_receive_alternate_message (:923-990) turns the 32-bit VDM_MSG_IRQ_STATUS flags into alternate_state
  bits and calls max77705_process_alternate_mode (:870-921), which issues the GET_VDM_RESP reads in order ID,
  SVIDS, MODES, ENTER, DP_STATUS, DP_CONFIGURE, ATTENTION. Missed-step recovery: if a later flag arrives without
  the earlier one, it re-reads the earlier one (:938-966). Skipped if host blocked / mdm_block (:882-890).
- max77705_vdm_message_handler (:694-763) dispatches on response byte vdm_data[1] (= VDM id echoed); ignores
  responses whose VDM header cmd_type == NAK(2) (:705-708). Response layout: [0]=opcode 0x4B (INFERRED: echo),
  [1]=VDM id, [2..3]=PD message header (copied to [0..1] for MODES/DP_STATUS/ATTENTION, :732-753),
  [4..7]=VDM header, [8..]=VDOs.
  - DISCOVER_IDENTITY: ID header VDO at [8] (USB VID = bits 15:0), Product VDO at [16] (PID = bits 31:16)
    (:714-722); resets is_sent_pin_configuration (:716).
  - DISCOVER_SVIDS (:359-436): scans SVIDs from [8] in 16-bit steps for 0xFF01; if found: switches the USB
    stack to HOST (MUIC attach rprd, USB ATTACH_DFP), sets dp_is_connect, dp_hs_connect=1 (hard-coded, USB HS
    only), raises CCIC_NOTIFY_ID_DP_CONNECT (VID/PID), waits host_turn_on_wait_q, then CCIC_NOTIFY_ID_USB_DP.
    No opcode is written here.
  - DISCOVER_MODES (:438-497): DP capability VDO at [8]; picks pin_assignment from UFP_D_Pin_Assignments
    (receptacle) or DFP_D_Pin_Assignments (plug) per Port_Capability; rejects DFP_D-only. THEN sends Enter Mode
    (set_DP_enter_mode_req) for any non-Samsung SVID (:492-494) -> **Enter Mode is AP-triggered**.
  - ENTER_MODE response (:499-515): only logs ACK/NAK. The DP Status Update request after Enter Mode is NOT sent by
    the AP -> the FW sends DP Status itself (INFERRED: no AP opcode exists for it; the AP receives a
    VDM_DP_STATUS_UPDATE flag next).
  - DP_STATUS (:556-612) and ATTENTION (:614-669): status VDO at [8]: if Port_Connected != 0 and pin config not
    yet sent: pin = select_pin(Multi_Function_Preference) (:517-554: multi-func -> prefer D,B,F,C,E,A; else
    C,E,A,D,B,F), then **AP sends DP Configure** with W_DATA = that mask; then HPD_State (bit 7) -> HIGH/LOW,
    HPD_Interrupt (bit 8) -> IRQ forwarded as CCIC_NOTIFY_ID_DP_HPD.
  - DP_CONFIGURE response (:671-692): waits device_add (HZ/2), raises CCIC_NOTIFY_ID_DP_LINK_CONF(pin).
    If SVID_1 == 0x04E8 also kicks Samsung DeX Discover Modes (not needed for a generic dongle).
- Alternate-mode enable (:1045-1185): OPCODE_SET_ALTERNATEMODE 0x55, 1 data byte, read_length 1.
  ALTERNATE_MODE_START -> mode 0x03 (MAXIM_ENABLE_ALTERNATE_SRC_VDM); STOP -> 0x01 (SRCCAP only, VDM off).
  Called from userspace via sysfs/ccic notifier with ALTERNATE_MODE_READY|START (alternate.h:375-379).
  On first boot (is_first_booting=1) START also writes REG_VDM_INT_M=0xF0 and REG_PD_INT_M=0x00 (:1165-1166).

## D. Interrupt mapping - CORRECTION to section A (drivers/mfd/max77705-irq.c)
- All USBC groups are read from the MUIC client (get_i2c :52-57), I2C_ADDR_MUIC = 0x4A>>1 = 0x25
  (mfd/max77705.c:58); PMIC top = 0xCC>>1 = 0x66 (:57), charger 0xD2>>1 = 0x69, FG 0x6C>>1 = 0x36 (:59-60).
- Mask regs: UIC_INT_M/CC_INT_M/PD_INT_M/VDM_INT_M (max77705-irq.c:35-38); mask bit 1 = masked
  (irq_mask sets the bit :163, unmask clears :175; all set to 0xFF at init :415-429).
- **The DP-specific VDM events are in PD_INT (0x04), not VDM_INT (0x05)** (max77705-irq.c:103-115):
  - PD_INT 0x04: b7 PDMSG, b6 PS_RDY, b5 DATAROLE, **b4 VDM_ATTENTION, b3 VDM_DP_CONFIGURE,
    b2 VDM_DP_STATUS_UPDATE**, b1 SSACCI, b0 FCTIDI.
  - VDM_INT 0x05: **b0 DISCOVER_ID, b1 DISCOVER_SVIDS, b2 DISCOVER_MODES, b3 ENTER_MODE** (b4-b7 unused ->
    VDM_INT_M init 0xF0 = enable exactly these four, ccic/max77705.h:99; alternate.c:1165).
  - UIC_INT 0x02: b7 APC_INT (= APCmdResI, opcode response ready), b6 SYSM_INT (sysmsg in 0x07) (:85-86).
  - The alternate.h enum VDM_MSG_IRQ_State bit positions (b4..b7) are software state, NOT register bits.
- Each VDM IRQ handler (usbc.c:2864-2954) just sets the matching software flag and calls
  max77705_receive_alternate_message; IRQs registered at usbc.c:3007-3089.
- IRQ thread (max77705-irq.c:216-377): read PMIC INTSRC 0x22 @0x66 (:216); bit3 IRQSRC_USBC
  (mfd/max77705-private.h:29-32). For PASS2+ one 4-byte bulk read of 0x02..0x05 from 0x25 (:319-320) -
  INT regs are clear-on-read (INFERRED from the read-once design and the PASS1 "intr reg clear issue" dummy
  reads :201, :294-313). Also re-reads 0x0B CC_STATUS1 and, if b0 Altmode == 0 while the AP has set alt mode,
  raises virtual IRQ ALTERROR (:353-358) -> max77705_vir_altmode_irq re-sends SET_ALTERNATEMODE
  (usbc.c:2956-2972). So **CC_STATUS1.b0 Altmode = "FW alt-mode enable is active"**; H1 read 0x08 -> Altmode 0
  -> the FW alt-mode engine is currently OFF (Android not running) (INFERRED, consistent).
- PMIC INTSRC_MASK 0x23 @0x66 bit3 = USBC source mask (usbc.c:3106-3137, max77705-irq.c:448-483). Only matters
  for the SoC IRQ line; a polling Windows driver can ignore it (INFERRED).

## E. Opcode transport (drivers/ccic/max77705_usbc.c)
- Write (max77705_i2c_opcode_write :1706-1745): buf[0]=opcode, buf[1..len]=data (len <= 32); ONE I2C bulk write
  to register 0x21 of length len+1 (:1732-1733), i.e. regs 0x21 (opcode), 0x22.. (data). If len < 32, then a
  single-byte write 0x00 to register 0x41 (OPCODE_WRITE_END) (:1735-1736) - this triggers execution
  (INFERRED: the 0x41 write is the "end of data" commit; with exactly 32 bytes the write reaches 0x41 itself).
- Completion: FW raises UIC_INT b7 APCmdResI -> max77705_apcmd_irq (:2806-2829) reads 0x07 USBC_STATUS2 then
  cmd_run dequeues the pending "response" node and calls max77705_irq_execute (:2164-2168).
- Read (max77705_i2c_opcode_read :1757-1785): ONE I2C bulk read from register 0x51, length read_length+1;
  data[0] = echoed opcode, data[1..] = payload (:1828-1832). Mismatch is logged, not fatal (:1834-1843).
- Queue (:2182-2229, :2352-2380): opcode_write enqueues {write, response} and runs immediately if idle;
  opcode_push only enqueues (runs after the in-flight command's APCmdRes). Strictly one command in flight.
  Stale-response timeout MAX77705_MAX_APDCMD_TIME (:2218-2225); OPCODE_WAIT_TIMEOUT 3000 ms (ccic/max77705.h:503).
- SET_VDM_REQ (0x48) response: data[1]==0xFF = invalid/not sent (:1914-1918); else data[2..5] = VDM header of
  the reply. Structured ACKs for ID/SVIDs/Modes/Enter are ignored here (the real data comes via the VDM IRQ +
  0x4B read) (:1926-1969). If an Attention arrives as the "response" while the last requested command (reg
  0x23 & 0x1F = VDM cmd byte of the last request) was Configure(17), the AP RETRIES DP Configure with
  W_DATA = 1 << (dp_selected_pin - 1) (:1970-1987).
- SET_ALTERNATEMODE (0x55) response: just sets set_altmode=1 (:2011-2013).
- Re-sent automatically after FW reset (recover list: CCCTRL1_W, FACTORY_TEST, SET_ALTERNATEMODE :1523-1568)
  and on sysmsg boot-done (REG_VDM_INT_M re-init at :2562, :2593).
- Masks at vendor probe: PD_INT_M=0x1C (DP status/configure/attention masked), VDM_INT_M=0xFF (usbc.c:3546-3547);
  alt-mode START (first boot) then sets VDM_INT_M=0xF0, PD_INT_M=0x00 (alternate.c:1165-1166).
  Init values after FW reset: UIC_INT_M 0x04, CC_INT_M 0x20, PD_INT_M 0x00, VDM_INT_M 0xF0 (usbc.c:2559-2562).
- **Android shutdown/reboot resets the USBC block**: max77705_usbc_shutdown writes all four INT_M = 0xFF and
  max77705_reset_ic = write reg **0x80 = 0x0F** + 100 ms (usbc.c:3692-3735, :2520-2524). After a reboot into our
  firmware/Windows the FW is therefore at its reset defaults: alt mode OFF (matches H1 CC_STATUS1 Altmode=0),
  INT_M = 0xFF or FW default, auto-VBUS = FW default (INFERRED; read 0x0E-0x11 to confirm - plain registers).
- Vendor alt-mode START only sends 0x55 if a READY was received first (check_is_driver_loaded, alternate.c:1106-1116);
  READY|START come from the Android USB/CCIC stack via pccic_data->set_enable_alternate_mode (usbc.c:3448) - the
  userspace caller was not traced (not needed: the chip-level effect is just OPCODE 0x55 data 0x03).

## F. Data-role cases (who sends Discover Identity)
- Phone SOURCE + DFP (bus-powered dongle, CC_STATUS0.CCStat=2): no AP Discover request anywhere in the vendor code
  -> the FW discovers by itself once alt mode is enabled (INFERRED from absence + the VDM IRQs arriving unprompted).
- Phone SINK + DFP (hub with its own PD charger, phone becomes DFP after a DR_Swap): the AP sends Discover Identity
  (0x48 + DP_DISCOVER_IDENTITY) itself - from the DataRole IRQ (PD_INT b5) when PD_STATUS1.b7 flips to DFP while
  SNK (max77705_pd.c:1313-1332), or at alt-mode START if already SNK+DFP (alternate.c:1123-1126, :1160-1164).
  After that the FW continues with SVIDs/Modes autonomously (INFERRED: AP never requests them).
- CC orientation for H3's SBU mux: CC_STATUS0 (0x0A) bits 7:6 CCPinStat (1 = CC1, 2 = CC2) (ccic/max77705.h:150-155,
  values NO_DETERMINATION 0 / CC1_ACTIVE 1 / CC2_ACTVIE 2 per enum ccic/max77705.h:254-263, decoded at
  max77705_cc.c:387-391).

## G. Dangerous writes (VBUS / charging / role) - do NOT issue in H2
- reg 0x80 = 0x0F on 0x25: USBC block reset (usbc.c:2522) -> drops the PD contract / charging renegotiation.
- OPCODE 0x54 FACTORY_TEST data 0x02/0x00 = auto-VBUS on/off (usbc.c:265-291); vendor disables it at every init
  (usbc.c:1516) and drives VBUS itself.
- OPCODE 0x37 SWAP_REQUEST (0x1 DR_SWAP, 0x2 PR_SWAP, 0x4 manual) (usbc.c:345-357); CCCTRL1_W 0x0C (CC SRC/SNK/DRP
  mode bits ccic/max77705.h:186-193); SRCCAP_REQUEST 0x32 / SET_SRCCAP 0x33 / APDO 0x3A / SET_PPS 0x3C; CHGIN_ILIM_W
  0x16/0x18; AFC_HV_W 0x20; QC2P0_SET 0x22 - all change power role or charger input V/I.
- VBUS OUT to a bus-powered dongle is NOT done by the max77705 FW (auto-VBUS off): on CCStat=SOURCE the kernel
  turns on the charger's OTG boost via power_supply "otg" ONLINE (max77705_cc.c:682-713 -> max77705_pd.c:479-560
  -> max77705_charger.c max77705_set_otg :800-848, CHG_CNFG_00 mode A/E/F BOOST_OTG_ON :1414-1437, charger 0x69).
  -> H2 with a bus-powered dongle needs a separately reviewed OTG-boost write (boot hazard rules apply); H2 with the
  UGREEN hub + its own PD charger avoids it (phone stays SINK, becomes DFP via DR_Swap) (INFERRED: hub behaviour
  UNVERIFIED until H1's plugged read).
- Safe in H2: OPCODE 0x55 (alt-mode enable), 0x48 (VDM requests), 0x4B (read VDM responses), INT_M writes
  0x0E-0x11, clear-on-read of 0x02-0x05.
- Byte packing of the 0x48 payloads checked mechanically (LSB-first bitfield packer, scratchpad h2chk/pack.py):
  Identity `09 01 80 00 FF`, Enter Mode `09 04 81 01 FF`, Configure `0A 11 81 01 FF 06 08 00 00` (pin D default;
  byte index 6 of the payload = 0x08 is what W_DATA replaces). Assumes GCC/ARM64 LE bitfield order (INFERRED,
  standard ABI).

## H. The H2 sequence (implementable, all on I2C 7-bit 0x25 unless noted)
Primitive OPC(op, data[n], rlen):  write [op, data...] to reg 0x21 (one burst, n <= 32); if n < 32 write 0x00 to
reg 0x41; wait for UIC_INT(0x02).b7 APCmdResI (or poll; 3000 ms budget); burst-read rlen+1 bytes from reg 0x51,
expect byte0 == op (usbc.c:1706-1785, :1828-1843, :2806-2829). One opcode in flight at a time (usbc.c:2182-2229).
Event poll: burst-read 0x02..0x05 (4 bytes, clear-on-read) and act on the bits below (max77705-irq.c:319-320).

0. (read-only pre-check) read 0x0A-0x0D and 0x0E-0x11. Expect CC_STATUS1(0x0B).b0 Altmode = 0 after a reboot
   (usbc.c:3692-3735 resets the block on Android shutdown).
1. Unmask (optional for polling; vendor values): UIC_INT_M(0x0E)=0x04, PD_INT_M(0x10)=0x00, VDM_INT_M(0x11)=0xF0
   (usbc.c:2559-2562, alternate.c:1165-1166). Whether masked bits still latch in 0x02-0x05 is UNVERIFIED - test by
   reading with masks as found first.
2. Enable FW alt mode: OPC(0x55, [0x03], 1) -> reply [55 xx] (alternate.c:1045-1056, :1119-1121;
   MAXIM_ENABLE_ALTERNATE_SRC_VDM alternate.h:536). Verify CC_STATUS1.b0 == 1 (max77705-irq.c:353-358).
3. If CC_STATUS0 CCStat == SINK (1) and PD_STATUS1.b7 DataRole == DFP: OPC(0x48, [09 01 80 00 FF], 10)
   (Discover Identity kick; pd.c:1329-1332, alternate.c:277-292). If SOURCE+DFP: nothing (FW discovers itself,
   INFERRED) - but a bus-powered dongle also needs VBUS (section G).
4. VDM_INT b0 (DISCOVER_ID): OPC(0x4B, [0x01], 31): [8..11] ID header (VID bits 15:0), [16..19] product VDO
   (PID bits 31:16) (alternate.c:711-725, :765-778).
5. VDM_INT b1 (DISCOVER_SVIDS): OPC(0x4B, [0x02], 31): scan 16-bit SVIDs from [8] for 0xFF01 (alternate.c:359-381).
   (The vendor loop bound uses Number_of_obj of a misaligned header - scan all of [8..31] instead, INFERRED.)
6. VDM_INT b2 (DISCOVER_MODES): OPC(0x4B, [0x03], 11): DP capability VDO at [8..11] (the vendor copies
   data[2..3] over data[0..1] only; the VDO stays at offset 8, alternate.c:730-734,
   :438-478; keep the raw bytes in the log). pin_assignment = UFP_D pins (byte 2 of VDO, bits 23:16) if receptacle,
   DFP_D pins (bits 15:8) if plug; Port_Capability (bits 1:0) must include UFP_D.
   Then **AP sends Enter Mode**: OPC(0x48, [09 04 81 01 FF], 10) (alternate.c:492-494, :294-308). The 0x48 reply
   only echoes (data[1]==0xFF = not sent) (usbc.c:1913-1969).
7. VDM_INT b3 (ENTER_MODE): OPC(0x4B, [0x04], 7): VDM header [4..7], cmd_type bits 7:6 == 1 -> ACK
   (alternate.c:499-515).
8. PD_INT b2 (DP_STATUS_UPDATE, sent by the FW after Enter Mode - INFERRED): OPC(0x4B, [0x10], 11): status VDO at
   [8..11] (after the [2..3]->[0..1] copy, alternate.c:740-744): Port_Connected 1:0, Multi_Function_Pref b4,
   HPD_State b7, HPD_IRQ b8 (alternate.h:299-315). If Port_Connected != 0 and not yet configured:
   pin = MFP ? first of D,B,F,C,E,A : first of C,E,A,D,B,F present in pin_assignment (alternate.c:517-554);
   **AP sends DP Configure**: OPC(0x48, [0A 11 81 01 FF 06 PP 00 00], 14), PP = one-hot pin (A 01 B 02 C 04 D 08
   E 10 F 20) (alternate.c:556-591, :310-325). HPD_State/HPD_IRQ -> HPD high/low/irq (:594-604).
9. PD_INT b3 (DP_CONFIGURE): OPC(0x4B, [0x11], 11): configure ACK -> DP link configured with pin PP
   (alternate.c:671-684) -> H3 (AUX switch, combo PHY lanes: C/E = 4-lane, D/B/F = 2-lane + USB3, d6 P2).
   If instead an Attention comes back as the 0x48 reply while reg 0x23 & 0x1F == 17, resend step 8's Configure
   (usbc.c:1970-1987).
10. Thereafter PD_INT b4 (ATTENTION): OPC(0x4B, [0x06], 11): DP status VDO at [8..11] -> HPD_State b7 (level),
   HPD_IRQ b8 (IRQ_HPD pulse) (alternate.c:614-669). This is the only HPD path (no SoC GPIO, d6 P2).
11. Detach: CC_INT b0 CCStatI with CC_STATUS0 CCStat = 0 -> forget state (max77705_cc.c:577-643). To stop: OPC(0x55,
   [0x01], 1) (ALTERNATE_MODE_STOP -> SRCCAP only, alternate.c:1128-1131).

## Sources (raw URLs, branch lineage-23.2)
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/ccic/max77705_alternate.c
- .../drivers/ccic/max77705_usbc.c, .../drivers/ccic/max77705_cc.c, .../drivers/ccic/max77705_pd.c
- .../include/linux/ccic/max77705_alternate.h, .../include/linux/ccic/max77705.h, .../include/linux/ccic/max77705_usbc.h
- .../drivers/mfd/max77705-irq.c, .../drivers/mfd/max77705.c, .../include/linux/mfd/max77705-private.h
- .../drivers/battery_v2/max77705_charger.c (OTG boost only)
(".../" = https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2)

## Progress log (cont.)
- 2026-10-18: all files fetched and read; sections A-H written. Nothing on the device touched. Open items for the
  H2 implementer: (1) does a masked INT bit still latch (test with masks as found); (2) FW default auto-VBUS after
  the shutdown reset (read-only check impossible - observe VBUS with a bus-powered dongle only after the OTG review);
  (3) hub behaviour (PD-powered -> SNK+DFP path) from H1's plugged read.
