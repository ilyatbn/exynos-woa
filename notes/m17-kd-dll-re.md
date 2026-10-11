# M17: reverse-engineering kd_8003_5143.dll (KDNET USB EEM, Synopsys backend)

Read-only analysis of `s10e-artifacts/win-w5/out/esp/EFI/Microsoft/Boot/kd_8003_5143.dll`
(22621.2506, 64480 bytes). Copied to a scratch dir; nothing in the repo changed except this file.

## Progress log

- P1: `symchk` fetched the public PDB (`kd_8003_5143.pdb`, GUID 7D538A50D199DF1C44BBBE2621085CE3, age 1).
  `llvm-pdbutil dump -publics` gives full function names. Sections: .text VA 0x1000, .rdata 0xB000,
  .data 0xC000. RVA = section VA + PDB offset. Key functions (RVA):
  KdInitializeLibrary 0x1BF0, KdInitializeController 0x1780, KdUsbFnInitializeLibrary 0x7BD8,
  KdUsbFnInitializeController 0x7920, KdUsbFnWaitForConnection 0x8038, ProcessEvents 0x8120,
  UsbFnMpSynopsysInitializeLibrary 0x5400, UsbFnMpSynopsysStartController 0x5600,
  UsbFnMpSynopsysStopController 0x5770, UsbFnMpSynopsysEventHandler 0x4F50,
  UsbFnMpSynopsysTimeDelay 0x58B0, WaitForDeviceEventOnEP 0x5C38, UsbFnMpChipideaInitializeLibrary 0x3160,
  UsbFnMpChipideaStartController 0x3300, CheckRestartUsbController 0x6220, IsLinkUp 0x44D8,
  IsLinkDown 0x44A8, KdUsbFnB* (bulk wrappers) 0x9320..0x9850, SetAttachConnectTimeouts 0x86D8.
  Globals: gOemData .data+0x2BC0 (RVA 0xEBC0), gOemDataLength RVA 0xEBA0, gEndpointWriteDelay RVA 0xC010,
  KdNetUsbFnMpExports RVA 0xEA80, KdNetExtensibilityImports RVA 0xEB08.
- P2: KdInitializeController @0x1780 (x0 = KDNET_SHARED_DATA; x9 = [x0+8] = DEBUG_DEVICE_DESCRIPTOR).
  `VendorID([x9+0xa]) == 0xFFFD` -> error. Backend selection on `OemData([x9+0xd0])` only if
  `OemDataLength([x9+0xd8]) >= 2`, by `u16 OemData[0]`:
  * 5 -> KdUsbFnB* frontend + UsbFnMpSynopsys* backend (0x17f4..0x1914)
  * 4 -> KdUsbFn* frontend + Synopsys backend (0x1918 -> 0x185c)
  * 3 -> KdUsbFnB* frontend + Chipidea backend (0x197c)
  * 1 / anything else / no OemData / len<2 -> KdUsbFn* + **Chipidea** silently (0x1a98). So a wrong
    type does silently fall back to ChipIdea (would poke ChipIdea regs at BAR0).
  Then tail-calls the chosen *InitializeController (0x1bbc).
  KdInitializeLibrary @0x1BF0 calls KdUsbFnInitializeLibrary twice (Chipidea then Synopsys) to size memory.
- P3: OEM phased-write function (anon) @0x4340 (x0=ctx, w1=phase). Requires gOemDataLength >= 0xC
  and u32 gOemData[+4]=='FIX2' (literal 0x46495832 at 0x44a0); count = u32 [+8], NOT bounded by
  length. Entry i at gOemData+0xC+16*i: byte0 BAR (0 -> ctx[0]=BAR0 VA, 1 -> ctx[8]=BAR1 VA,
  0xFF -> Stall(1000us) via imports+0x28, else skip), byte1 phase (must == w1), +4 offset,
  +8 AND, +0xC OR; write(base+off, (read & AND) | OR) via imports+0x40/+0x60. Callers: 0x3dc8,
  0x47d8, 0x4bf4 (phases TBD).
- P4: UsbFnMpSynopsysStartController @0x5600: ctx[0]=Desc+0x20 (BaseAddress[0].TranslatedAddress),
  ctx[8]= Desc+0x38 if BaseAddress[1].Valid(Desc+0x31) else 0 -> **second BAR is mapped/used** only by
  the FIX2 table (byte0=1). ctx[0x10]=DMA mem VA, ctx[0x18]=its PA (imports+0x20). Copies OemData to
  gOemData once if len<0x100. Then calls core init @0x4630.
  UsbFnMpSynopsysTimeDelay @0x58b0 derives elapsed time from **DSTS.SOFFN (bits 16:3)** - i.e. time
  only advances when the controller sees SOFs from a host. UsbFnMpSynopsysStopController @0x5770:
  clears DCTL bit31, acks GEVNTCOUNT(0xC40C), polls DSTS bit22 (DEVCTRLHLT) with a perf-counter
  timeout (imports+0xd0) - bounded.
- P5: Core reset/init (anon "ResetController") @0x4630, called at end of StartController (0x5748):
  1. read DCTL(0xC704); if bit31 RUN_STOP set: write DCTL&0x7fffffff twice, then **poll DSTS(0xC70C)
     bit22 DEVCTRLHLT until set, NO timeout** (0x4714..0x4728). (Ours: DCTL=0x00F00000 -> skipped.)
  2. DCTL |= 0x40000000 (CSFTRST) then **poll DCTL bit30 until clear, NO timeout** (0x47b8..0x47cc).
  3. FIX2 table phase 1 (0x47d8).
  4. GCTL(0xC110) = (GCTL & ~0x1000) | 0x12000 (PRTCAPDIR=device, bit16 U2RSTECN).
  5. GEVNTADRHI(0xC404)=PA hi, GEVNTADRLO(0xC400)=PA lo, GEVNTSIZ(0xC408)=(v&0x7fff0000)|0x1000
     (4 KiB, interrupt unmasked), GEVNTCOUNT(0xC40C)=v&0xffff0000.
  6. DCFG(0xC700) = (DCFG & 0xFFBFFFFC) | 4 -> DEVSPD=4 (SuperSpeed), LPM_CAP(bit22) cleared.
  7. DEVTEN(0xC708) |= 0x25F.
  8. EP cmd helper 0x3e90(ep0, cmd 9 = DEPSTARTCFG); 0x40b8 x2 (EP0 OUT/IN SETEPCONFIG);
     0x4c48(ctx,2,1) (EP0 setup TRB start).
  9. FIX2 table phase 2 (0x4bf4) -> our {BAR0, phase2, 0xC700, AND 0xFFFFFFF8, OR 0} turns DEVSPD to 0 (HS).
  10. DCTL |= 0x80000000 RUN_STOP; return. No wait after RUN_STOP.
  No GSNPSID (0xC120) read, no GUSB2PHYCFG/GUSB3PIPECTL access, nothing beyond BAR0+0xC800 region seen so far.
- P6: DEPCMD helper @0x3e90 (ctx, ep, cmd, par0, par1, par2): reg = BAR0 + 0xC800 + ep*16.
  **Before** issuing: loop {read DEPCMD(+0xC); Stall(gEndpointWriteDelay)} while bit10 CMDACT set -
  **NO timeout**. Then PAR0(+8), PAR1(+4), PAR2(+0), DEPCMD = cmd|0x400. Never waits for completion
  after issue (next command waits). Variant @0x3f90 same loop. gEndpointWriteDelay default 100 us
  (.data 0xC010 = 0x64), override via load option `SYNOPSYS_ENDPOINT_DELAY=` (clamped 20..2000).
- P7: ConnectionDone handler @0x3c68 (only after a host reset/connect): reads DSTS speed bits[2:0];
  SS(4): GCTL RAMCLKSEL(&~0xC0)|=0x40, GUSB3PIPECTL(0xC2C0)|=0x40, EP0 mps 512; HS(0)/FS/LS: GCTL
  bits7:6 =0, GUSB2PHYCFG(0xC200)|=0x20000, mps 64/8. Then FIX2 phase 3 (0x3dc8), reconfigure EP0.
  So phases are: 1 = right after CSFTRST, 2 = before RUN_STOP, 3 = on ConnectDone.
- P8: KdUsbFnBInitializeController @0x95e0 = memset Rx/Tx ring (28 slots) then tail into
  KdUsbFnInitializeController @0x7920 ("B" = buffered wrapper; same hardware path as type 4).
  KdUsbFnInitializeController: 0x8c40 (calls backend StartController via exports+0x18, allocates
  28 transfer buffers), then loop 0x7af4: EventHandler(ctx,0,&none) until "no event" (bounded: drains
  pending events). Then sets attach/connect timeouts (100/1 if bus speed 3..4 else 10/0, 0x7b3c) and
  calls KdUsbFnWaitForConnection @0x8038.
  Generic EventHandler @0x6830 message ids (jump table 0x6dd8): 1 setup, 2/3 EP done, 4 reset,
  5 detach, 6 attach, 9 speed/ConnectDone, 10 link "cable down", 11 "cable on".
- P9: KdUsbFnWaitForConnection @0x8038: polls EventHandler every 100 us (Stall imports+0x28) until
  "connected" flag (+0x28) or attach timeout (+0x2660) elapsed; extends once by +0x2664 if attached
  (+0x26). SetAttachConnectTimeouts @0x86d8: 10 s/10 s for Intel PCI (0x8086, class 0xC), otherwise
  **3 s / 3 s** (0x2DC6C0 us). Bounded; returns success even without a host. ProcessEvents @0x8120
  bounded (64 iterations). Rx path 0x8178 bounded at 1,000,000 us (0xF4240), all counted in Stall()
  units (not wall clock).
- P10: Generic Tx path bounded: KdUsbFnGetTxPacket @0x7740 (1 retry, 10 us), WaitForTxPacketToBeSent
  @0x90c0 (100 x 10 us), 0x9160 / 0x8178 (1 s in 100 us stalls). So generic KdUsbFn* layer has no
  unbounded loop; the unbounded loops are ALL in the Synopsys backend:
  (a) DSTS.DEVCTRLHLT wait @0x4714 (only if RUN_STOP was set at entry),
  (b) DCTL.CSFTRST self-clear wait @0x47b8,
  (c) DEPCMD.CMDACT pre-issue waits @0x3edc (helper 0x3e90), 0x3fe0 (0x3f90), 0x4eb0/0x4f04
      (ConfigureEnableEndpoints). Each iteration stalls gEndpointWriteDelay (100 us), no counter.
- P11: Synopsys EventHandler @0x4f50 reads GEVNTCOUNT(0xC40C), parses dev events (bit0=1):
  type 0x000 DISCONN->msg5, 0x100 USBRST->msg4 (via 0x3ae0, which issues DEPCMD ENDXFER (8)/CLEARSTALL(5)
  on endpoints - more CMDACT waits), 0x200 CONNDONE->0x3c68, 0x300 ULSTCHNG->link up/down (msgs 10/11),
  0xC00 -> skip 4 extra words. Acks by writing consumed bytes to GEVNTCOUNT.
- P12: Complete BAR0 register set touched by Synopsys backend (all `mov #imm` in 0x3660..0x6230):
  0xC110 GCTL, 0xC200 GUSB2PHYCFG (only |=0x20000 on HS ConnectDone), 0xC2C0 GUSB3PIPECTL (only |=0x40 on
  SS ConnectDone), 0xC400..0xC40C GEVNT*, 0xC700 DCFG, 0xC704 DCTL, 0xC708 DEVTEN, 0xC70C DSTS,
  0xC710/0xC714 DGCMDPAR/DGCMD (cmd 2 = SET_PERIODIC_PARAMS for SET_SEL, no completion wait),
  0xC720 DALEPENA, 0xC800+16n DEPCMD. **No GSNPSID (0xC120) read, no ID check, nothing outside
  0xC000..0xCFFF, no QSCRATCH/glue access.** (0xC268..0xC280 seen are ctx struct offsets, not MMIO.)
  It never clears GUSB2PHYCFG.SUSPHY(bit6)/ENBLSLPM(bit8) or GUSB3PIPECTL.SUSPHY(bit17) - unlike Linux
  dwc3 which clears SUSPHY before every DEPCMD and (since 2024, "usb: dwc3: core: Prevent phy suspend
  during init") during init because a set SUSPHY causes core soft reset timeouts.
- P13: Correction of the m17-usb-kd.md breadcrumb: the `cmp w0,#4 / cmp w0,#3` right after the FIX2
  literal are IsLinkDown @0x44a8 / IsLinkUp @0x44d8 (args: saved DSTS speed, ULSTCHNG link state) - NOT
  controller types. Controller type is selected only in KdInitializeController (P2); 5 is valid.
- P14: Our UEFI KD path (Beyond0LteUsbHostDxe/Dwc3.c lines ~176-212, Dwc3CoreInit) leaves
  GUSB2PHYCFG.SUSPHY(bit6)=1, GUSB2PHYCFG.ENBLSLPM(bit8)=1, GUSB3PIPECTL.SUSPHY(bit17)=1. The DLL never
  clears any of them. With RUN_STOP=0 and no host, a suspended UTMI PHY stops the PHY clock -> DCTL.CSFTRST
  may never self-clear (P5 step 2) and/or DEPSTARTCFG never completes so the next DEPCMD's CMDACT pre-wait
  (0x3edc) spins forever (Linux dwc3_send_gadget_ep_cmd clears SUSPHY before every EP command for this
  reason; databook 2.60a 6.3.2.5). INFERRED - leading hang candidate, not device-verified.
- P15 recommendations:
  1. UEFI KD path: clear GUSB2PHYCFG bits 6 and 8 before handoff (keep USB3 SUSPHY set while the SS
     PHY is held down - INFERRED). Optionally pre-run CSFTRST with a timeout in UEFI and record the
     outcome in the 'USBD' status area, so the next boot tells which wait would hang.
  2. DBG2 FIX2: add phase-1 entry {BAR 0, phase 1, off 0xC200, AND 0xFFFFFEBF, OR 0} (WriteCount 2) -
     runs after CSFTRST, before every DEPCMD. Phase-2 DCFG entry stays. 0xFF BAR entries = 1 ms stall.
  3. Type 5 is right (4 = same backend without Rx/Tx ring); any other value silently goes to ChipIdea.
  4. BAR1 is only used by FIX2 entries with byte0=1; pointing it at the USB PHY block would allow PHY
     writes in phases 1/2/3 (PHY address UNVERIFIED).
