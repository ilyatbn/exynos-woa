# M12 WiFi M7 - rx data path audit (no rx completions)

Task: research-only audit of why firmware consumes all 128 rxpost buffers but never
writes the D2H RX completion ring (driver 1.0.7.2). Compare our driver with vendor DHD
and brcmfmac. Sources: `drivers/Exynos9820Wlan/*` (this worktree), DHD/brcmfmac under
the session scratchpad `src/bcmdhd`, `src/brcmfmac`.

## Progress log
- started: notes file created.
- host_cap decode: our 0x00801407 = API ver 7 | HOSTRDY 0x400 | DS_NO_OOB_DW 0x1000 | UR_FW_NO_TRAP 0x800000
  (bit defs: bcmdhd/include/bcmpcie.h:402-426; our compute: WlanMsg.c:1253-1256).
  DHD dhd_set_host_cap (dhd_msgbuf.c:3071-3190) for rev>=6 additionally ALWAYS sets
  HOSTCAP_EXTENDED_TRAP_DATA 0x20000 (dhd_msgbuf.c:3100), and conditionally IDMA/IFRM/DAR/EDL/PKT_TXSTATUS.
  DHD also writes DNGL_TO_HOST_TRAP_ADDR (fw_trap_buf pa) right after (dhd_msgbuf.c:3184-3186).
  None of the missing bits is an rx-completion mode switch.
- flags2 0x40801001 decode (bcmpcie.h:131-157): EXTENDED_TRAP_DATA 0x1, EDL_RING 0x1000,
  bit 0x00800000 = undefined in this DHD's header, D2H_D11_TX_STATUS 0x40000000.
- pcie_shared flags 0x30040007 (notes/m12-wifi-m2.md:151) = v7 | D2H_SYNC_XORCSUM 0x40000 | HOSTRDY_SUPPORT 0x10000000 |
  NO_OOB_DW 0x20000000. No DMA_INDEX 0x10000, no IDMA 0x400000, no IFRM, no DAR, no 2BYTE_INDICES
  (bcmpcie.h:67-124). => DHD's own logic (dhd_pcie.c:8616-8646) keeps ALL r/w indices in TCM, d2h_sync_mode =
  XORCSUM; DMA-index buffers are never allocated (dhd_pcie.c:8696-8724 gated on dma_*_ring_upd_support/IDMA).
  Our TCM-index reads are what DHD does. RULED OUT: index location.
- Index slot layout (dhd_fillup_ring_sharedptr_info dhd_pcie.c:8851-8945): h2d slots 0,1 = ctrl, rxpost; d2h
  slots 0,1,2 = ctrl cpl, tx cpl, rx cpl; flowrings continue the h2d arrays at slot 2. Ours WlanMsg.c:1271-1277
  identical. RULED OUT.
- Ring geometry / item layout: bcmmsgbuf.h:47-70 (rxpost 32 B x 512, rxcpl 40 B x 512 for rev7) and
  host_rxbuf_post_t / host_rxbuf_cmpl_t (bcmmsgbuf.h:905-959) == ours (Exynos9820Wlan.h:303-312, 543-551).
  RULED OUT.
- rxpost item: dhd_prot_rxbuf_post dhd_msgbuf.c:4597-4616 sets msg_type, if_id 0, epoch seq%253, flags =
  current_phase, data_buf_len = pkt len, metadata 0 when rx_metadata_offset 0. Ours (WlanMsg.c:466-475 via
  H2dNext 256-266) identical except flags 0 (phase not advertised: we do not set HOSTCAP_H2D_VALID_PHASE, so fw
  must not check it). RULED OUT (low).
- sup_wpa: firmware has no in-dongle supplicant (m7 notes: sup_wpa -> -23), so fw cannot be eating EAPOL M1
  itself. fmac sets sup_wpa 0 explicitly only when FWSUP feature exists (cfg80211.c:2490-2514). RULED OUT.
- Data buffer is below 4 GB, 4 KB aligned (WlanMsg.c:951-955). Same DMA enabler as control buffer.
- fmac (this tree) never writes host_cap at all (rg host_cap/HOSTCAP in brcmfmac pcie.c/msgbuf.c: none);
  hostready = DB1 write 1 when flags HOSTRDY_DB1 0x10000000 (pcie.c:921-926), called from preinit
  (pcie.c:1467). Ours same (WlanMsg.c:1303-1306). So host_cap is not what a working fmac depends on.
- fmac rx posting (msgbuf.c:935-1004, 1008-1022, 1650-1660): posts rx data buffers FIRST at attach (before
  event / ioctlresp bufs), count = max_rxbufpost from shared (pcie.c:1650-1653; = 511 here), 2048 B each
  (msgbuf.c:56), refills when outstanding <= max - 32 (msgbuf.c:1028-1035). DHD: posts in bursts of 32 up to
  max_rxbufpost (dhd_msgbuf.c:4438-4475; 480 of 511 here), size = fw iovar "rxbufpost_sz" queried in
  dhd_sync_with_dongle BEFORE posting (dhd_msgbuf.c:4199-4219), then preinit ioctls.
  DIFFERENCE: we post 128 (RX_BUF_COUNT, Exynos9820Wlan.h:558; WlanMsg.c:997-1005) and never query rxbufpost_sz.
- pktid: fmac rx ids 0..1023 (msgbuf.c:52,1641), DHD small ids; ours 0x3000+i - opaque 32-bit in both refs.
- ORDER (key finding): ours posts the rx data buffers AFTER WLC_UP and after escan:
  WLC_UP at WlanMsg.c:1382-1383, DataInit (rx posts) at WlanMsg.c:1423 (resident hand-over).
  DHD: dhd_bus_start (dhd_linux.c:9253) -> dhd_sync_with_dongle (dhd_linux.c:9418) -> rxbufpost_sz query +
  dhd_msgbuf_rxbuf_post (dhd_msgbuf.c:4199-4220) -> dhd_preinit_ioctls (dhd_msgbuf.c:4225, which also issues
  WLC_DOWN) -> WLC_UP only later from wl_cfg80211 (e.g. wl_cfg80211.c:1825/19635).
  fmac: rings (pcie.c:2188) -> msgbuf rx params (pcie.c:2214-2215) -> brcmf_attach (pcie.c:2220) whose msgbuf
  attach fills ALL rx data buffers first (msgbuf.c:1655-1665) before any firmware ioctl/UP.
  => in both references the firmware does wl UP with its host rx-buffer pool already full; ours does UP with
  zero host rx buffers. Hypothesis (UNVERIFIED, no firmware source): in SPLITRX mode 4 the d11 rx FIFO 0
  (data) descriptors are only filled from host buffers at UP / rx-interrupt time; mgmt frames (scan, auth,
  assoc) still work via dongle memory, but data frames (EAPOL M1) find FIFO 0 empty -> dropped. Fits: fw r 128
  (fw pulled the posts into its pool) yet rx cpl w 0 and console silent.
- Mailbox: our driver never reads d2h_mb_data (shared+0x2C ptr) nor PCIMailBoxInt (0xC30) (rg: only the log at
  WlanMsg.c:1189). DHD services both (dhdpcie_read_handle_mb_data). Control path keeps working, so low rank.
- Counters for diagnosis: GET_VAR "counters" carries rxnobuf (wlioctl.h:3803 WLC counters) and rxf0ovfl (macstat,
  wlioctl.h:4299/4738); xtlv format, WL_CNT_VERSION_XTLV 30 (wlioctl.h:3479), WLC block id 0x100, ucode macstat
  0x200/0x400 (wlioctl.h:3499-3506). A rising rxf0ovfl / rxnobuf across the join = rx FIFO 0 starved.

## Findings summary (ruled out)
index location (TCM, flags 0x30040007), slot layout, ring geometry/item layout, XORCSUM epoch, rxpost field
contents, pktid range, fw supplicant, buffer < 4 GB, D2H ring-config / soft doorbell (DHD only with
DHD_D2H_SOFT_DOORBELL_SUPPORT), interface-create messages (fmac/DHD send none for the primary STA on PCIe),
EAPOL as event (WLC_E_EAPOL_MSG 25 exists in fweh.h:51 but neither driver uses it; EAPOL is a plain RX_CMPLT
data frame in both).

## Ranked code changes (most likely first)
1. Post rx data buffers BEFORE WLC_UP. Allocate the data buffer + RxPost loop (DataInit minus flowring) right
   after the ioctl-resp buffers / hostready (fmac order) or at least before step 8's WLC_UP (WlanMsg.c:1383).
   Stop path must then free the data buffer after ARM halt + BME off. Cheapest A/B test without moving code:
   at the start of WlanMsgJoin, WLC_DOWN (3) then WLC_UP (2) (wlioctl_defs.h:604-605; WLC_DOWN not yet in
   Exynos9820Wlan.h, only WLC_UP :405) so the firmware
   re-runs UP with 128 buffers in its pool.
2. Post max_host_rxbufs buffers like the references (fmac 511, msgbuf.c:1016 / pcie.c:1651; DHD 480 in bursts
   of 32, dhd_msgbuf.c:4444-4466), refill when outstanding <= max-32 (fmac msgbuf.c:1032, dhd :3767). Needs
   RX_BUF_COUNT 511 -> rx region 511 x 2048 = 0xFF800, DAT_DMA_SIZE 0x26000 + 0x100000 = 0x126000.
3. GET_VAR "rxbufpost_sz" (u16) before posting and use it as data_buf_len (dhd_msgbuf.c:4199-4219; fall back
   to 2048 on error, refuse > DHD_FLOWRING_RX_BUFPOST_PKTSZ_MAX). Log the value - if > 2048 our buffers are
   too small for this fw.
4. Diagnostics in the join-end dump: GET_VAR "counters" hex (rxnobuf / rxf0ovfl), *(d2h_mb_data_ptr),
   PCIMailBoxInt 0xC30.
5. host_cap: add HOSTCAP_EXTENDED_TRAP_DATA 0x20000 (DHD always, dhd_msgbuf.c:3100) -> 0x00821407, and write
   host_trap_addr (shared+0x58, sh_addr_t) = a host buffer (dhd_msgbuf.c:3184-3186). Not an rx switch; only
   improves trap diagnostics. No other missing bit applies (no IDMA/IFRM/DAR/HSCB/EDL support needed).
- audit complete; ranked list above. Nothing built or run (research only).
