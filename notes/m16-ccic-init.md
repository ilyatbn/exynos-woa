# M16 - MAX77705 CCIC (USBC) init after Linux shutdown - code reading

Source: LineageOS android_kernel_samsung_exynos9820 lineage-18.1, files in %TEMP%
(max77705_usbc.c, max77705_cc.c, max77705.h; more fetched as needed).
Problem: post-Linux-reboot, CC_STATUS0 (0x0A) = 0x48 with a Rd hub attached, never cc_SOURCE.

## Progress log

- Started. Probe = max77705_usbc.c:3369-3587. Shutdown = max77705_usbc.c:3688-3731.
- Shutdown (usbc.c:3688): muic_shutdown; mask_irq; write PD_INT_M/CC_INT_M/UIC_INT_M/VDM_INT_M = 0xFF
  (comment says "unmask" but 0xFF = all masked); then unless WATER or RID attached:
  max77705_reset_ic (usbc.c:2520: write reg 0x80 <- 0x0F), masks again 0xFF x4, read UIC_INT (clear).
- Probe order (usbc.c:3522-3584): get_version_info -> init_irq_handler -> muic_probe -> cc_init -> pd_init
  -> write REG_PD_INT_M=0x1C, REG_VDM_INT_M=0xFF (3542-3543) -> max77705_init_opcode(usbc,0) (3544)
  -> cc_booting_complete=1 -> usbc_umask_irq (3578) -> cc_open_req=1; pdic_manual_ccopen_request(0) (3584).
  op_ctrl1_w default = BIT_CCSrcSnk|BIT_CCSnkSrc|BIT_CCDetEn (3486).
- Resolved constants (max77705.h): REG_UIC_INT_M=0x0E, CC_INT_M=0x0F, PD_INT_M=0x10, VDM_INT_M=0x11
  (h:82-85); *_INIT: UIC 0x04, CC 0x20, PD 0x00, VDM 0xF0 (h:96-99). Opcode mailbox: write 0x21, end 0x41,
  read 0x51 (h:511-513). Opcodes (h:519-576): CTRL1_R/W 05/06, CTRL3_R/W 09/0A, CCCTRL1_R/W 0B/0C,
  CCCTRL2_R/W 0D/0E, CCCTRL3_R/W 0F/10, CTRLREG_INIT_R/W 1A/1B, CTRLREG3_R/W 1C/1D, SAMSUNG_FACTORY_TEST 54,
  SET_ALTERNATEMODE 55, FW_OPCODE_CLEAR 70. CCCTRL1 bits (h:188-194): SrcCurCh b7, SrcCur b6:5, CCSrcSnk b4,
  CCSnkSrc b3, CCDbgEn b2, CCAudEn b1, CCDetEn b0. cc_pin_state enum (h:~360): 0 none,1 SINK,2 SOURCE,3 Audio,
  4 Debug,5 Error,6 Disabled. SYSMsg: 0x03 BOOT_WDT, 0x05 BOOT_POR (in USBC_STATUS2 0x07).
- usbc_umask_irq/mask_irq (usbc.c:3102-3133) are NOT USBC-block registers: they RMW PMIC top-level reg 0x23
  (usbc_data->i2c = PMIC 0x66 client) bit3 (USBC source mask in INTSRC_MASK). Shutdown's mask_irq sets bit3.
  These only gate the INTB line to the AP. INFERRED: masks don't gate CCIC FW state machine.
- init_opcode(usbc,0) (usbc.c:1511): disable_auto_vbus = opcode 0x54 data [0x00] (usbc.c:279-291);
  if pdata->support_audio: CCCTRL1_W 0x0C data [op_ctrl1_w | DbgEn | AudEn] = 0x19|0x06 = 0x1F (usbc.c:293-307).
  reset arg 0 so no SET_ALTERNATEMODE at probe.
- pdic_manual_ccopen_request(0) after cc_open_req=1 (usbc.c:3583-3584 -> 1006-1062): queues CTRLREG3_R (0x1C,
  read 1 byte), and if LockErrorEn (bit1) set, CTRLREG3_W (0x1D) [ctrl3 & ~0x02]. The "set" branch (cc open)
  writes CTRLREG3_W with bit1=1 then CCCTRL2_W (0x0E) [0x84] = CCForceError (usbc.c:972-982).
- reset_ic (usbc.c:2520-2524): reg 0x80 <- 0x0F, msleep(100). Same reset used on SYSERROR_BOOT_WDT /
  SYSMSG_BOOT_POR followed by INT_M = INIT values, read UIC_INT, init_opcode(1), umask (usbc.c:2545-2600).
- cc_init (cc.c:744-873): only requests IRQs; then max77705_ccstat_irq_handler(INIT_DETECT) (cc.c:854) which
  reads CC_STATUS0 and acts ONLY if ccstat==cc_SINK (cc.c:560-564); reads CC_STATUS1 ConnStat (water). No writes.
- pd_init (pd.c:1405): no register writes (grep write_reg/update_reg in pd.c: none).
- Source attach path (cc.c:681-712): case cc_SOURCE -> if current_pr != SRC: current_pr=SRC and, if previous
  role was NONE/SINK, max77705_vbus_turn_on_ctrl(ON) (pd.c:479). That only calls the "otg" power_supply
  ONLINE=1 (pd.c:~540) -> charger driver. No USBC opcode for VBUS. auto_vbus_en is false (init_opcode sent
  0x54 [0x00]) so the AP, not CCIC FW, switches VBUS. I.e. CCStat=SOURCE comes FIRST, then VBUS (AP-driven).
- cc_No_Connection (cc.c:614-642): vbus_turn_on_ctrl(OFF).
- VBUS enable (charger, max77705_charger.c:797-857 set_otg): CHG_INT_MASK (0xB1) |= CHG_IM(b4)|CHGIN_IM(b6), BYP_IM(b0) cleared;
  then chg_set_mode_state(OTG_ON): CNFG_00 (0xB7) MODE[3:0] -> 0xA (BOOST_OTG_ON) from mode 0, or 0xE
  (BUCK_BOOST_OTG_ON) from mode 4 (buck on, i.e. buck normally on) (charger.c:1405-1500; charger.h:157,161).
  Observed 0x1E under TWRP = mode 0xE + bit4 (INFERRED WDTEN). From UNO modes it first writes MODE=4, waits 1-2 ms.
  OTG ILIM: charger_initialize writes CNFG_02 (0xB9) bits[7:6]=01 (900 mA) (charger.c:924-927; charger.h:183-186);
  also CNFG_00 OTG_CTRL bits cleared at init (charger.c:920-922).
- MFD i2c probe (max77705.c:987-1124): read PMICID1/PMICREV -> max77705_usbc_fw_setting(0) (max77705.c:1098,937)
  -> max77705_usbc_fw_update (624): writes PD/CC/UIC/VDM_INT_M = 0xFF (660-665), reads FW_REV/FW_MINOR; if no
  update needed: "Don't need to update!" -> out (no reset, no opcodes). Then max77705_irq_init, mfd_add_devices.
  So at usbc probe entry the 4 USBC INT_M regs are all 0xFF - same as after shutdown. Linux therefore proves the
  CCIC works fine from the post-shutdown masked state up to that point.
- FW update path (only when FW older): masks, chg mode changes, opcode 0xD0 via write 0x21=0xD0, 0x41=0 (secure
  update entry), reset 0x80<-0x0F + 100ms. Not relevant for 1A if Linux's bin is not newer (INFERRED).
- MFD irq_init (max77705-irq.c:~415-488): writes 0xFF to SYS/CHG/UIC/CC/PD/VDM INT_M; PMIC INTSRC_MASK (0x23)
  |= 0x0F, then clear CHG bit, set USBC bit (USBC still masked at top level until usbc probe umask_irq).
  genirq bus_sync_unlock (max77705-irq.c:128-145) writes irq_masks_cur to each group INT_M whenever a nested IRQ
  is requested/enabled -> the real UIC/CC INT_M values at runtime = bits cleared for every request_threaded_irq
  in usbc/cc/pd/muic (cc_init requests all 7 CC bits except... see cc.c:772-850), NOT REG_*_INT_M_INIT. Explicit
  probe writes then override PD_INT_M=0x1C, VDM_INT_M=0xFF (usbc.c:3542-3543).
- IRQ thread (max77705-irq.c:~290-330): bulk-reads UIC_INT..VDM_INT (0x02..0x05, read-to-clear) on every USBC
  source IRQ; workaround: if VBUSDetI and CCStat==0 && VBUS low -> fake CCStatI.
- beyond0lte DT (arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_eur_open_25.dts:1357-1367): max77705@66 has
  max77705,support-audio, fw_product_id=2, support_pd_role_swap -> probe DOES send CCCTRL1_W [0x1F]
  (DRP SrcSnk+SnkSrc, DbgEn, AudEn, DetEn; CCSrcCur bits 6:5 = 00).
- muic_probe (muic.c:2395-2540): init_regs reads UIC_INT (0x02) to clear (muic.c:2294-2310), requests UIC irqs
  b0-b5 (muic.c:2118-2152); init_detect -> detect_dev(INIT) (may write CTRL1_W 0x06 switch path, muic.c:330-335)
  and enable_detecting_short = opcode 0x56 [0x07] (muic.c:492-512, non-factory builds).
- muic_shutdown -> com_to_open: CTRL1_W 0x06 [COM_OPEN] (the D+/D- switch already handled in boot #79).
- Runtime INT_M (INFERRED from requested irqs, irq.c:85-101): UIC_INT_M=0x00 (APC b7,SYSM b6 + muic b0-b5),
  CC_INT_M=0x20 (DETABRT b5 never requested) = REG_CC_INT_M_INIT, PD_INT_M=0x1C, VDM_INT_M=0xFF (later 0xF0).
- SET_ALTERNATEMODE 0x55: data 0x03 (SRC_VDM) / 0x01 (SRCCAP) (max77705_alternate.h:534-536, alternate.c:1044-1150);
  only sent at probe-time via DP driver / after BOOT_POR (init_opcode(1)). Not needed for plain Type-C attach.
- ccopen callers: only probe (0) and muic hiccup_store (0) (muic.c:1022) -> nothing in this tree SETS
  LockErrorEn/CCForceError on shutdown. But probe still unconditionally reads CTRLREG3 and clears bit1 if set.

## Analysis (q2) - post-shutdown state
- Reset 0x80<-0x0F reboots the USBC MCU (100 ms). Linux's own model of a CCIC reboot is the SYSMSG_BOOT_POR /
  BOOT_WDT handler (usbc.c:2545-2600): reset, INT_M := INIT (0x04/0x20/0x00/0xF0), read UIC_INT (clear),
  init_opcode(1) = 0x54[00], 0x0C[1F], 0x55[03]; then unmask top-level. That is the closest documented
  "bring CCIC from fresh boot to working" sequence.
- No code waits for/sends an "AP ready" handshake; no CTRLREG_INIT (0x1A/0x1B) or CCCTRL3 opcode is used
  anywhere in probe. INT_M and PMIC 0x23 bit3 only gate INTB (INFERRED - datasheet not available).
- Observed 0x48: CCPinStat=CC1, CCVcnStat=1, CCStat=0. VCONN on + pin resolved suggests the FW saw Rd on CC1
  but did not reach Attached.SRC (INFERRED). Candidates: (a) FW in WATER state (CC_STATUS1 0x0B bit1 ConnStat=1 ->
  source role suppressed; shutdown even skips the reset in WATER), (b) AttachWait.SRC stuck because VBUS not
  vSafe0V (CC_STATUS1 bit3 VSAFE0V=0), (c) AttachSrcErr (CC_STATUS1 bit2), (d) CCCTRL1 after reset lacking
  CCSrcSnk/CCSnkSrc (sink-only / no DRP) - Linux always rewrites 0x1F, (e) LockErrorEn set (CTRLREG3 bit1).

## Recommended UEFI sequence (q4) - every step INFERRED unless marked
Each opcode: write 0x21=[op, data..], 0x41=0; poll UIC_INT (0x02) bit7 APCmdResI (read clears); read 0x51.. (byte0 = op echo).
0. Diagnose first (read-only): 0x07 USBC_STATUS2 (SYSMsg; 0x05 BOOT_POR / 0x03 WDT), 0x0B CC_STATUS1 (b1 ConnStat=WATER,
   b2 AttachSrcErr, b3 VSAFE0V), opcodes 0x0B CCCTRL1_R, 0x0D CCCTRL2_R, 0x0F CCCTRL3_R, 0x1C CTRLREG3_R (b1 LockErrorEn).
1. Read 0x02..0x05 (clear latched ints incl. SYSMsgI from the reset).
2. INT_M (Linux-equivalent, harmless): 0x0E=0x04, 0x0F=0x20, 0x10=0x1C (or 0x00), 0x11=0xFF.
3. 0x54 [0x00] (auto-VBUS off; AP drives OTG) - verify response echo 0x54.
4. 0x0C [0x1F] CCCTRL1_W (DRP + DetEn + Aud/Dbg) - verify with 0x0B read = 0x1F.
5. 0x1C read; if data[1] bit1 set -> 0x1D [data[1] & ~0x02].
6. (optional) 0x56 [0x07] short detection, as Linux.
7. Poll CC_STATUS0 0x0A until CCStat (b2:0)==2 (SOURCE); on SOURCE: charger 0xB9 (CNFG_02) bits7:6=01 (900 mA),
   0xB1 |= 0x50, &= ~0x01, 0xB7 MODE[3:0]: 0x4 -> 0xE (or 0 -> 0xA). Verify BC_STATUS 0x08 b7 VBUSDet=1.
8. If 0x0A stays 0x48 after step 4: escalate (a) 0x80<-0x0F, 100 ms, repeat 1-5 (Linux's BOOT_POR path);
   (b) quick test only: 0x54 [0x02] auto-VBUS (CCIC switches VBUS itself; kernel has it commented out, usbc.c:3547).
