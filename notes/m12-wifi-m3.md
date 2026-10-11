# M12 WiFi milestone 3 — msgbuf rings, DMA, first firmware ioctl (cur_etheraddr / ver)

Base: main e3f72a2 (milestone 2 device-validated: BCM4375 fw 18.41.117 runs, pcie_shared v7 at
0x0024F8BC 180 ms after release, notes/m12-wifi-m2.md RESULT). Worktree branch, no phone access in
this session (the main session runs the device test).

Goal: from the running firmware, set up the msgbuf common rings, enable bus mastering, send
MSGBUF_TYPE_IOCTLPTR_REQ GET "cur_etheraddr" and "ver", get IOCTL_ACK + IOCTLRESP back in the D2H
control-complete ring (polled), log MAC + version + console. Any failure: BME off, ARM halted.

## Tasks (one commit each, planned)

1. Notes + references: pcie_shared v7 / rings_info / ring memory / msgbuf message layouts with
   file:line; the DMA decision for this platform (inbound path, SysMMU, coherency).
2. Driver: DMA enabler + common buffers, ring setup, BME, ioctl, console; DriverVer bump; build.
3. Test procedure + MILESTONES line.

## Progress log

- 2026-10-12: worktree fast-forwarded to main e3f72a2 (`git merge --ff-only main`).
- Reference trees already in this session's scratchpad from M2 (src/bcmdhd = LineageOS
  android_kernel_samsung_exynos9820 bcmdhd, src/brcmfmac = torvalds master). Missing for M3:
  dhd_msgbuf.c, brcmfmac msgbuf.c/commonring.c/msgbuf.h/commonring.h, the Exynos PCIe RC driver
  (inbound / SysMMU). Fetching next.
- Fetched (read-only, scratchpad src/, not committed): bcmdhd lineage-23.2 dhd_msgbuf.c (11676 lines),
  dhd_proto.h, include/wlioctl.h; brcmfmac master msgbuf.c (1722), msgbuf.h, commonring.c/.h.
  Re-fetched dhd_pcie.c, include/bcmpcie.h, include/bcmmsgbuf.h, brcmfmac pcie.c: byte-identical (cmp)
  to the M2 copies, so every M2 citation still holds.

## Protocol facts (dhd = bcmdhd lineage-23.2, fmac = brcmfmac master; file:line)

pcie_shared v7 (`pciedev_shared_t`, dhd include/bcmpcie.h:330-395; members are uint32 / sh_addr_t =
2 x uint32, so no padding):

| off | member | use in M3 |
|---|---|---|
| 0x00 | flags | device: 0x30040007 = v7 + D2H_SYNC_XORCSUM 0x40000 (bcmpcie.h:78) + HOSTRDY_SUPPORT 0x10000000 (:115) + NO_OOB_DW 0x20000000 (:118). NOT set: DMA_INDEX 0x10000 (:67), 2BYTE_INDICES 0x100000 (:82), DAR 0x80000000 (:124), IDMA, IFRM |
| 0x14 | console_addr | console dump (M2) |
| 0x20/0x22 | total_lfrag_pkt_cnt / max_host_rxbufs (u16) | logged (fmac MAX_RXBUFPOST_OFFSET 34, pcie.c:227) |
| 0x24 | dma_rxoffset | logged. dhd applies it to rx data / event packets only, NOT to ioctl responses (dhd_prot_ioctcmplt_process copies from offset 0, dhd_msgbuf.c:6205-6300). fmac adds it to every D2H item (msgbuf.c:1387). M3 follows dhd; a non-zero value is logged as a warning |
| 0x28/0x2C | h2d_mb_data_ptr / d2h_mb_data_ptr | logged only (no D3/DS handshake in M3) |
| 0x30 | rings_info_ptr | -> ring_info_t |
| 0x34 | host_dma_scratch_buffer_len | host writes 8 (DMA_D2H_SCRATCH_BUF_LEN dhd_msgbuf.c:119; dhd_prot_init :3304-3307; writeshared dhd_pcie.c:4260-4275) |
| 0x38 | host_dma_scratch_buffer (sh_addr_t) | host writes the scratch bus address |
| 0x40 | host_scb_addr | untouched (HSCB off). fmac writes a "ringupd" buffer at 64/68 (pcie.c:236-237) = this member in v7: fmac legacy, NOT copied |
| 0x50 | flags2 | logged |
| 0x54 | host_cap | host writes (dhd_set_host_cap dhd_msgbuf.c:3071-3190, called :3270; writeshared HOST_API_VERSION dhd_pcie.c:4361) |
| 0x58 | host_trap_addr | NOT written (dhd writes its fw_trap_buf, :3184-3185) -> HOSTCAP_EXTENDED_TRAP_DATA must not be advertised |

host_cap the driver writes (registry `HostCap` overrides; 0xFFFFFFFF = do not write at all = fmac style, fmac
never writes host_cap): active ipc version min(host 7, fw 7) = 7 (dhd :3082-3087) | HOSTCAP_H2D_ENABLE_HOSTRDY
0x400 if flags has HOSTRDY_SUPPORT (:3089-3092; dhdpcie_bus_get_pcie_hostready_supported dhd_pcie.c:10133 returns
d2h_hostrdy_supported) | HOSTCAP_DS_NO_OOB_DW 0x1000 (:3095) | HOSTCAP_UR_FW_NO_TRAP 0x800000 (:3137)
= **0x00801407**. Left out of dhd's set: EXTENDED_TRAP_DATA 0x20000 (no trap buffer), FAST_DELETE_RING (no
flowrings), TXSTATUS_METADATA, IDMA/IFRM/DAR/HSCB/EDL/HP2P (fw flags clear or not needed). Constants
bcmpcie.h:403-418.

ring_info_t (bcmpcie.h:290-324): ringmem_ptr 0x00, h2d_w_idx_ptr 0x04, h2d_r_idx_ptr 0x08, d2h_w_idx_ptr 0x0C,
d2h_r_idx_ptr 0x10, four sh_addr_t host index arrays 0x14..0x33 (DMA_INDEX only: unused here),
max_tx_flowrings 0x34, max_submission_queues 0x36, max_completion_rings 0x38, max_vdevs 0x3A (u16).
fmac struct brcmf_pcie_dhi_ringinfo pcie.c:393-406 agrees. dhd rejects 0 for any of the three maxima
(dhd_pcie.c:8701-8714).

ring_mem_t (bcmpcie.h:275-282), 16 B, one per common ring at ringmem_ptr + 16*ringid; ring ids H2D ctrl submit 0,
H2D rxpost 1, D2H ctrl cpl 2, D2H tx cpl 3, D2H rx cpl 4 (bcmpcie.h:171-175; dhd_fillup_ring_sharedptr_info
dhd_pcie.c:8851-8880): idx u16 +0, type u8 +2, rsvd +3, max_item u16 +4, len_items u16 +6, base_addr +8.
Host writes base_addr, max_item, len_items (dhd_prot_ring_init dhd_msgbuf.c:8292-8318 -> writeshared
RING_BUF_ADDR/RING_MAX_ITEMS/RING_ITEM_LEN dhd_pcie.c:4322-4340; fmac brcmf_pcie_alloc_dma_and_ring
pcie.c:1131-1158).

Indices (no DMA_INDEX -> in TCM), rw_index_sz = 4 (dhd_pcie.c:8597-8598): H2D ring i (0,1): w at
h2d_w_idx_ptr + 4i, r at h2d_r_idx_ptr + 4i; D2H ring 2+j: w at d2h_w_idx_ptr + 4j, r at d2h_r_idx_ptr + 4j
(dhd_pcie.c:8884-8908; bcmpcie.h:208-243; fmac pcie.c:1318-1345). Both drivers access an index as a 16-bit TCM
word (dhd writeshared/readshared RING_WR_UPD/RING_RD_UPD wtcm16/rtcm16 dhd_pcie.c:4341-4350, 4402-4410; fmac
write_tcm16 pcie.c:1270). Host zeroes w/r of every common ring at init (dhd :8311-8314).

Item sizes / depths (dhd include/bcmmsgbuf.h:47-77; fmac msgbuf.h:10-24): H2D ctrl 40 B x 64, H2D rxpost 32 B x
512 (fmac 1024), D2H ctrl cpl 24 B x 64, D2H tx cpl 24 B x 1024 (v7; 16 pre-v7), D2H rx cpl 40 B x 512 (fmac
1024; 32 pre-v7). M3 uses the dhd depths.

Doorbells = PCIE2 core registers via the BAR0 window at the PCIE2 core base (0x18003000, M2 EROM). DAR is
off (flags bit 31 clear) -> dhd: H2D DB0 = PCIH2D_MailBox 0x140, DB1 = PCIH2D_DB1 0x144 (pcie_core.h:946-947;
dhd_bus_db0_addr_get/db1 dhd_pcie.c:440-458), value 0x12345678 (ringbell :7786, hostready :6160).
fmac, PCIE2 rev >= 64: 0xA20/0xA24 (pcie.c:149-150, 452-459, 2478) = the DAR copies. PCIE2 is rev 64 here
(M2 EROM) -> driver follows dhd (fw says DAR off); registry `DoorbellDar`=1 switches to 0xA20/0xA24.
Hostready: dhd rings DB1 after common-ring init + scratch buffer, before posting ioctl-resp buffers
(dhd_prot_init dhd_msgbuf.c:3331-3332; hostrdy_after_init set TRUE unconditionally at attach dhd_pcie.c:685);
fmac writes h2d_mailbox_1 = 1 in preinit (pcie.c:921-925, 1467).
Mailbox interrupt mask (PCIMailBoxMask) is left as reset left it -> no INTx path is used (polled mode).

Messages (dhd include/bcmmsgbuf.h; fmac msgbuf.c:76-160 has identical layouts):
- cmn_msg_hdr_t (bcmmsgbuf.h:145-156): msg_type u8, if_id u8, flags u8, epoch u8, request_id u32.
- H2D epoch = seqnum % 253, seqnum starts 254 (H2D_EPOCH_MODULO/INIT_VAL bcmmsgbuf.h:44-45; dhd_msgbuf.c:8129,
  5065); H2D ctrl flags = current_phase = 0 at init (h2d_sync_init dhd_msgbuf.c:1356-1357), toggles on wrap.
- ioctl_req_msg_t (bcmmsgbuf.h:313-330), 40 B: hdr, cmd u32 +8, trans_id u16 +12, input_buf_len +14,
  output_buf_len +16, rsvd[3] +18, host_input_buf_addr +24, rsvd1[2] +32. Type IOCTLPTR_REQ 0x9 (:172),
  request_id DHD_IOCTL_REQ_PKTID 0xFFFE (dhd_msgbuf.c:1670); one request buffer of DHD_IOCTL_REQ_PKTBUFSZ 2048,
  input clamp 2048-40 (dhd_msgbuf.c:114-115; dhd_fillup_ioct_reqst :8079-8160).
- ioctl_resp_evt_buf_post_msg_t (bcmmsgbuf.h:333-343): hdr (type IOCTLRESP_BUF_POST 0xB, request_id = host
  pktid), host_buf_len u16 +8, reserved[3], host_buf_addr +16, rsvd[4]. dhd posts 8 of 8192 B
  (DHD_FLOWRING_MAX_IOCTLRESPBUF_POST / DHD_FLOWRING_IOCTL_BUFPOST_PKTSZ dhd_msgbuf.c:128,132;
  dhd_prot_rxbufpost_ctrl :4886-5110) plus 32 event buffers (:131). M3 posts ioctl-resp buffers only.
- D2H control completions, 24 B, last word = dma_done_t marker (bcmmsgbuf.h:584-756): compl_msg_hdr_t
  {status s16, ring_id u16} at +8. IOCTLPTR_REQ_ACK 0xA (ioctl_req_ack_msg_t :695-705, cmd +12),
  IOCTL_CMPLT 0xC (ioctl_comp_resp_msg_t :679-692: resp_len u16 +12, trans_id u16 +14, cmd u32 +16),
  GEN_STATUS 0x1, RING_STATUS 0x2 (write_idx +12), WL_EVENT 0xE. Response payload = first resp_len bytes of
  the posted buffer whose pktid == request_id (dhd :6290-6296).
- XORCSUM sync (dhd_prot_d2h_sync_xorcsum dhd_msgbuf.c:1003-1080; bcm_compute_xor32 bcmutils.h:758-765): an item
  is complete when epoch == seqnum % 253 (seqnum starts D2H_EPOCH_INIT_VAL 254, bcmmsgbuf.h:41-42 -> first
  epoch 1) AND the xor of its 6 words == 0; else re-read after a stepped delay.
- dhd_prot_init order (dhd_msgbuf.c:3198-3390): host_cap (:3270) -> ring_init x5 (:3279-3296) -> d2h/h2d sync
  init -> scratch addr+len (:3304-3307) -> hostready DB1 (:3331-3332) -> ioctl-resp + event buffer posts
  (:3387-3388). dhd_bus_init (dhd_pcie.c:8949-9030) = readshared + intr enable before it; BME on since probe.
- fmac fwil.h fetched: BRCMF_C_GET_VERSION 1 (fwil.h:14), BRCMF_C_GET_VAR 262 (fwil.h:80) -> WLC_GET_VAR 262 with
  the iovar name NUL-terminated at the start of the request buffer; "cur_etheraddr" returns 6 B, "ver" a string.
- Vendor RC + SysMMU sources fetched (scratchpad src/rc): drivers/pci/dwc/pcie-exynos-host-v0.h (379 lines),
  drivers/iommu/exynos-pcie-iommu.c (1341) / .h (505); v0.c = drivers/pci/dwc/pcie-exynos-host-v0.c (M1 copy).

## DMA decision (task item 2) - how the dongle reaches host RAM on this platform

Path: BCM4375 PCIe master -> Exynos PCIe0 RC (DesignWare) inbound -> SysMMU_PCIE (0x13C20000) -> FSYS1 bus
(sharability from SYSREG_FSYS1+0x700) -> DRAM.

1. **Inbound iATU: none, identity.** The vendor kernel programs only OUTBOUND viewports (CFG0/CFG1 v0.c:1133-1167,
   MEM v0.c:1191-1213; grep of v0.c for INBOUND/inbound: no hits). Beyond0LtePcieDxe does the same (PcieLink.c
   ProgramAtu :266-293, outbound region1 CFG0 + region0 MEM only). A DesignWare RC passes an inbound TLP whose
   address matches no inbound region through untranslated, which is what Android runs on -> PCI bus address ==
   AXI address. **No DBI window and no DSDT change are needed for M3.**
2. **RC bus master: already on.** The RC forwards upstream memory requests only with its own COMMAND.BME set;
   Beyond0LtePcieDxe SetupRc writes IO|MEM|MASTER|SERR (PcieLink.c:226-227, from the generic dw_pcie_setup_rc).
   The EP's COMMAND.BME (bit 2) is the only switch the driver flips (CFG0 window, already in _CRS).
3. **SysMMU: bypass, UNVERIFIED reset state.** The board DT enables SysMMU for pcie_0 (`use-sysmmu = "true"`,
   beyond0lte_common.dtsi:2320) - under Android DMA addresses are IOVAs. Our UEFI never touches the SysMMU
   (grep SysMmu/SYSMMU in Beyond0LtePcieDxe: none). The vendor driver's "off" state is CTRL = 0 = full bypass
   (exynos-pcie-iommu.c:154-166 "PCIe SysMMU use full disable(by pass) as default", CTRL_DISABLE 0
   exynos-pcie-iommu.h:104); pcie_1 on this board is configured `use-sysmmu = "false"` (dtsi:2328; that port is
   `status = "disabled"` here, so this is only a hint that the vendor treats bypass as a normal mode). That REG_MMU_CTRL (0x13C20000+0) resets to 0 is
   **UNVERIFIED**; it closes on the device: an ioctl round trip (fw DMA-reads our H2D item, DMA-writes the D2H
   completion + response) proves bypass. If the fw instead traps or the H2D read index never moves, the next
   read is REG_MMU_CTRL / REG_MMU_STATUS (+0x08) from UEFI (SysMMU page 0x13C20000 is in Exynos9820.h,
   EXYNOS_PCIE0_SYSMMU_BASE) - do not guess, read it.
4. **Coherency: device declared non-coherent.** UEFI sets SYSREG_FSYS1+0x700 bits 9:8 (CacheCoherencyEnable
   PcieLink.c:118-131, kernel enable_cache_cohernecy v0.c:1885-1910, use-cache-coherency=true dtsi:2321), so EP
   traffic is shareable (snooping). The DSDT gives WLAN `_CCA 0` (Dsdt.asl:381), so Windows allocates common
   buffers NON-CACHED. Non-cached CPU mappings are correct whether or not the interconnect snoops -> no cache
   maintenance in the driver, and the P5 coherency question does not gate M3. Barriers: KeMemoryBarrier before
   every TCM index write / doorbell and after every TCM index read, before touching ring memory.
5. **Address width: 32-bit.** WdfDmaEnablerCreate with WdfDmaProfilePacket (32-bit) -> common buffers below
   4 GB (DRAM starts 0x80000000). The dongle takes 64-bit addresses (sh_addr_t) but the RC/AXI width above 4 GB
   is unread on this path, so the driver does not depend on it. No `_DMA` object -> logical address ==
   physical address.
6. **Allocation:** one WdfCommonBuffer (page-aligned) carved into 4 KB-aligned regions: H2D ctrl 64x40,
   H2D rxpost 512x32, D2H ctrl cpl 64x24, D2H tx cpl 1024x24, D2H rx cpl 512x40, ioctl request 2048, scratch 8,
   8 ioctl-response buffers x 8192 -> 0x24000 B. The DMA enabler is created in EvtDeviceAdd (its failure is
   reported, never fails the device); buffers are created in PrepareHardware and deleted before it returns.
7. **Lifetime rule (no DMA into freed memory):** at the end of the M3 run - success or failure - the driver
   halts the ARM CR4 (WlanDl halt sequence), waits 1 ms, clears EP COMMAND.BME, and only then deletes the
   common buffer. So when PrepareHardware returns: BME off, ARM halted (M2 left the fw running; with
   `Parameters\Msgbuf` = 0 the M2 behaviour is kept unchanged). Recorded behaviour change vs M2.

- Task 1 committed next: this notes file + MILESTONES M3 line (doing). No DSDT/UEFI change needed (DMA decision 1-2).

## Review of commit 49a8cec (task 1)

- Checked mechanically: every cited file:line printed with a sed script (scratchpad chk1.sh) against the
  fetched trees + this worktree. Commit stat: 2 files (notes + MILESTONES, one new line, no other line
  touched).
- FOUND, fixed in the follow-up commit: five wrong citations - host_trap_addr write :3189 -> :3184-3185;
  DS_NO_OOB_DW :3094 -> :3095 and UR_FW_NO_TRAP :3141 -> :3137 (dhd_msgbuf.c); the max-rings checks
  dhd_pcie.c:8673-8686 -> :8701-8714; rw_index_sz :8601-8602 -> :8597-8598; and "RC BME = kernel 0x146
  v0.c:500" was wrong (v0.c:500 is chk_epconf_access writing the EP's command in a debug test) - the RC
  command comes from the generic dw_pcie_setup_rc, cited via PcieLink.c only.
- Claim downgraded during the commit: pcie_1 `use-sysmmu=false` is on a port that is status disabled.

## Task 2 progress (driver)

- Exynos9820Wlan.h: M3 constants (shared/ring_info/ring_mem offsets, ring geometry, message layouts, doorbells,
  common-buffer carve-out 0x23000 B, MSG_* result codes), each cited. WlanDl.h: WLAN_BUS hand-over struct,
  WLAN_FW_UP_FN callback, Msgbuf/HostCap/DoorbellDar settings, WLAN_MSG_CTX. WlanDl.c: ConsoleDump can dump
  only the bytes since the previous dump; WlanBusBpRd/BpWr/ArmHalt/ConsoleDump; WlanDlRun calls the callback
  after "DL 0" while BAR0/BAR2 are mapped. Next: WlanMsg.c, WlanProbe.c (DMA enabler in DeviceAdd).
- BLOCKED: writing drivers/Exynos9820Wlan/WlanMsg.c (the milestone-3 core: common-buffer carve-out, ring_mem /
  index / host_cap / scratch writes, BME on, hostready + DB0 doorbells, IOCTLRESP_BUF_POST, IOCTLPTR_REQ GET_VAR
  "cur_etheraddr" / "ver", D2H control-complete polling with XORCSUM sync, the always-run stop path) was stopped
  by a safety classifier mid-write; nothing of it reached disk and it was not retried. The DMA enabler in
  EvtDeviceAdd (WlanProbe.c) was not written either (it was the step after WlanMsg.c).
- Committed instead (task 2a, plumbing only, no behaviour change): the M3 constants, the WLAN_BUS hand-over and
  WLAN_FW_UP_FN callback in WlanDlRun (WlanProbe passes NULL, so M2 runs exactly as before), the
  Msgbuf/HostCap/DoorbellDar config fields (not read from the registry yet), ConsoleDump "since last dump".
  The draft WLAN_MSG_CTX / WlanMsgRun declarations were removed again (no definition exists).
- Build of the plumbing: build-package.ps1 -Out scratchpad\wlan-m3-plumbing: exit 0, /W4 /WX clean, signed,
  catalog generated. NOT a test artifact (DriverVer unchanged 1.0.2.0, no new function): nothing to flash or swap.
- Next order of work: WlanMsg.c per "Protocol facts" + "DMA decision" above (dhd_prot_init order; lifetime rule 7),
  DMA enabler (WdfDmaProfilePacket, 0x23000 B common buffer) in EvtDeviceAdd, registry reads for
  Msgbuf/HostCap/DoorbellDar, DriverVer 1.0.3.0, report header string fix ("milestone-1 probe"), device test.

## Review of commit 72ce05c (task 2a plumbing)

- Checked: ConsoleDump with *FromIdx == 0xFFFFFFFF prints the same last-N bytes as before (M2 unchanged);
  WlanProbe passes FwUp = NULL so the callback block is dead until WlanMsg.c exists; region offsets in
  Exynos9820Wlan.h do not overlap (each end <= next start, total 0x23000); message field offsets recomputed from
  the bcmmsgbuf.h structs (ioctl_req 40 B with host addr at +24; buf post addr at +16; ioctl_cmplt resp_len +12,
  trans_id +14, cmd +16, marker +20).
- FOUND: WlanDl.h documents Msgbuf "default 1" and HostCap "WLAN_HOSTCAP_AUTO (default)", but WlanReadConfig
  zero-fills the struct, so both were 0. Fixed in the follow-up commit (defaults set; still not read from the
  registry - that belongs with WlanMsg.c). Rebuilt: exit 0, /W4 /WX clean.

### 2026-10-12 - WlanMsg.c written (main session; subagent write was blocked)
- `drivers/Exynos9820Wlan/WlanMsg.c` + `WlanMsg.h`: WlanMsgRun = fw-up callback. Order: flags check
  (v7, XORCSUM, no DMA_INDEX/2BYTE) -> ring_info -> WdfCommonBuffer 0x23000 (checked 4 KB aligned, < 4 GB)
  -> host_cap (AUTO = 7 | DS_NO_OOB_DW | UR_FW_NO_TRAP | HOSTRDY if supported) -> ring_mem[0..4] + indices
  zeroed + scratch -> BME on -> DB1 hostready -> 8 IOCTLRESP_BUF_POST, DB0 -> GET_VAR cur_etheraddr (out 32),
  ver (out 200), polled 5 ms up to 2 s each -> console dump -> stop: ARM halt, 1 ms, BME off, free.
- WlanProbe.c: device context with WDFDMAENABLER (WdfDmaProfilePacket, MaximumLength MSG_DMA_SIZE; failure ->
  MSG_NO_DMA, driver still loads), registry Msgbuf/HostCap/DoorbellDar read, FwUp = WlanMsgRun.
- `FILE_PAGE_ALIGNMENT` does not exist in the WDK headers -> FILE_QUAD_ALIGNMENT (the common buffer is page
  aligned by allocation; WlanMsgRun checks La & 0xFFF).
- DriverVer 1.0.3.0. Build: s10e-artifacts\wifi-m3\Exynos9820Wlan, sys sha256 prefix 7F4ED72F402B8A80.
- Review: item offsets vs the constants table (IOREQ_BUF_ADDR 0x18, BUFPOST_ADDR 0x10, CPL_* in a 24 B item);
  D2H index slot = ring id - 2; IOCTL_CMPLT request_id = response-buffer pktid (fmac
  brcmf_msgbuf_process_ioctl_complete looks it up in rx_pktids), handled as 0x1000+k. No findings.
- Device test pending: expect `[msg] IOCTL CMPLT status 0` x2, `cur_etheraddr = ..`, `ver = wl0: ...`, `MSG 0`.
- Run m3-1 (02:12): boot #93 (s10e-artifacts\boot93-v2.img sha 77bfd211…, = main c20e6c0: audio M2 tone +
  watchdog 1) flashed, readback OK; Exynos9820Wlan.sys 1.0.3.0 (7f4ed72f…) copied over the DriverStore copy
  in exynos9820wlan.inf_arm64_1828e8231b692d7b (NTFS needed fsck.ntfs first: unclean after the forced reset).
  Battery 63 %. Unattended cycle started.
- Run m3-1 RESULT (02:15, back in TWRP after 150 s, collect ran; Windows clock ~3 min behind):
  shared flags2 0x40801001, ring_info 0x002B80BC (ringmem 0x002B8108, max sub 43 cpl 5, flowrings 40);
  common buffer at logical 0xEFF66000; host_cap 0x00801407 read back; fw consumed all 8 resp-buffer posts
  (r 8); GET_VAR cur_etheraddr -> ACK 0, CMPLT 0 len 32 pktid 0x1007 -> **A8:DB:03:06:3C:EF**; GET_VAR ver ->
  CMPLT 0 len 200 pktid 0x1006 -> `wl0: Jul 19 2022 10:52:21 version 18.41.117 (B1 Network/rsdb) FWID
  01-1b4772fd`; `MSG 0`; stop: ARM halted, EP COMMAND 0x00100002 (BME off). No firmware console output
  during the exchange. Windows: no PnP problem other than the parked CPUs 6/7. **Milestone 3 done.**
  Firmware uses response buffers LIFO (0x1007 then 0x1006).
- Next (M4): event buffers (WL_EVENT), rx-post ring, flowring create, and interrupts (INTx/MSI) instead of
  polling; then an NDIS miniport shell. The firmware must stay halted between exchanges until a resident
  DMA lifetime exists.
