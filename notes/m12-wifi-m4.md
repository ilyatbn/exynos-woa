# M12 WiFi milestone 4 — firmware resident + first scan (escan results over the event path)

Goal: keep the BCM4375 firmware running for the device's lifetime (rings + DMA buffer owned by the device,
torn down in ReleaseHardware), post event buffers, bring the interface up and run one escan; log every
BSS (BSSID, SSID, channel, RSSI) the firmware reports. Proves the radio receives. Still polled, no NDIS.

## Plan (tasks, one commit each)
1. Research + constants (this file, Exynos9820Wlan.h) - cited.
2. Restructure: WlanDlRun keeps BAR0/BAR2 mapped and the msgbuf state alive past PrepareHardware
   (device-context owned); ReleaseHardware = ARM halt, BME off, free, unmap.
3. Event buffers (EVENT_BUF_POST) + WL_EVENT handling in the D2H ctrl poll; polling via a passive
   system thread / WDF timer + work item while in D0.
4. WLC_UP, event_msgs mask with ESCAN_RESULT, escan start; parse escan results; report file + registry.
5. Device test, MILESTONES line.

## Progress log
- 2026-10-12: fetched (scratchpad src/, read-only, not committed): bcmdhd lineage-23.2
  include/wlioctl_defs.h (2463 lines), include/bcmevent.h (1287), include/ethernet.h, include/802.11.h,
  wl_cfg80211.c (24627); brcmfmac master fwil.h, fweh.h, fweh.c, fwil_types.h, cfg80211.c, common.c.
  Already present from M2/M3: bcmmsgbuf.h, bcmpcie.h, wlioctl.h, dhd_msgbuf.c, dhd_pcie.c, msgbuf.c.
- bcmmsgbuf.h:160-189 message types: EVENT_BUF_POST 0xD, WL_EVENT 0xE, TX_POST 0xF, TX_STATUS 0x10,
  RXBUF_POST 0x11, RX_CMPLT 0x12, INFO_BUF_POST 0x19.
- Event path facts:
  - EVENT_BUF_POST uses the same item as IOCTLRESP_BUF_POST (`ioctl_resp_evt_buf_post_msg_t`,
    bcmmsgbuf.h:333-343: hdr 8, host_buf_len u16 @8, reserved[3], host_buf_addr @16, rsvd[4]).
  - Count/size: fmac msgbuf.c:60 BRCMF_MSGBUF_MAX_EVENTBUF_POST 8, :56 BRCMF_MSGBUF_MAX_PKT_SIZE 2048, used
    for event buffers at :959 (dhd posts 32, dhd_msgbuf.c:131, with DHD_FLOWRING_RX_BUFPOST_PKTSZ, whose
    define is not in the fetched files). M4 uses 8 x 2048 (fmac).
  - WL_EVENT D2H item `wlevent_req_msg_t` (bcmmsgbuf.h:708-721): hdr 8 (request_id = posted buffer pktid,
    dhd_prot_event_process +12), compl_hdr (status s16 @8, ring id @10), event_data_len u16 @12, seqnum u16
    @14, rsvd u32 @16, marker @20 (24 B item). The buffer is consumed, so re-post one per event (fmac :1126).
    dhd pulls rx_dataoffset (= shared dma_rxoffset, 0 on this fw per the M3 run) before use.
- Event packet (fmac fweh.h:202-248, all big-endian): eth hdr 14; brcm_ethhdr @14 (subtype be16, length be16,
  version u8, oui[3] = 00 10 18 @19, usr_subtype be16 @22 = 1 BCMILCP_BCM_SUBTYPE_EVENT); event msg @24:
  version be16 @24, flags @26, event_type be32 @28, status be32 @32, reason @36, auth_type @40, datalen be32
  @44, addr[6] @48, ifname[16] @54, ifidx @70, bsscfgidx @71; data @72.
- Ioctl numbers (fmac fwil.h): UP 2 (:15), DOWN 3 (:16), SET_PASSIVE_SCAN 49 (:39), GET_VAR 262 (:80),
  SET_VAR 263 (:81). Event ESCAN_RESULT = 69 (fweh.h:89). "event_msgs" iovar get/set of the mask
  (fmac common.c:414-431, fweh.c:462).
- escan (fmac fwil_types.h, natural alignment, little-endian):
  - brcmf_escan_params_le (:460): version u32 @0 (BRCMF_ESCAN_REQ_VERSION 1, :73), action u16 @4,
    sync_id u16 @6, params @8.
  - brcmf_scan_params_le (:368): ssid_le {len u32, 32 B} @0, bssid[6] @36, bss_type s8 @42
    (DOT11_BSSTYPE_ANY 2, :72), scan_type u8 @43 (ACTIVE 0, :62), nprobes @44, active_time @48,
    passive_time @52, home_time @56, channel_num @60, channel_list @64.
  - brcmf_escan_result_le (:470): buflen u32 @0, version u32 @4, sync_id u16 @8, bss_count u16 @10,
    bss_info @12.
  - brcmf_bss_info_le (:314), offsets computed under natural alignment and still to be cross-checked
    against fmac cfg80211.c usage:
    - version @0, length @4, BSSID @8, beacon_period @14, capability @16, SSID_len @18, SSID[32] @19;
    - rateset {count u32 @52, rates[16] @56}, chanspec u16 @72, atim @74, dtim u8 @76, RSSI s16 @78,
      phy_noise @80, n_cap @81, nbss_cap @84, ctl_ch @88;
    - ie_offset @116, ie_length @120, SNR @124 (size 128).
- Cross-check closed: dhd `wl_bss_info_v109_t` (wlioctl.h:384-424, explicit padding fields) gives the same
  offsets:
  - BSSID @8, SSID_len @18, SSID @19, rateset @52/56, chanspec @72, dtim @76, RSSI s16 @78, ctl_ch @88;
  - ie_offset u16 @116, ie_length u32 @120, SNR @124.
- escan start (fmac cfg80211.c:1447-1488, run_escan + escan_prep :~1100):
  - v1 path when the fw lacks FEAT_SCAN_V2: version 1 (wlioctl.h:871 ESCAN_REQ_VERSION 1), action 1
    (wlioctl_defs.h:307 WL_SCAN_ACTION_START), sync_id;
  - params v1: bssid ff*6, bss_type 2, scan_type 0 (active), nprobes/active/passive/home = -1, channel_num 0
    (all channels), empty ssid;
  - size 8 + 64 (BRCMF_SCAN_PARAMS_FIXED_SIZE fwil_types.h:50 = dhd WL_SCAN_PARAMS_FIXED_SIZE wlioctl.h:907).
  - Sent as SET_VAR "escan".
- Results: event 69 with status 8 PARTIAL per batch (bcmevent.h:330), final status 0 SUCCESS (fweh.h:118).
  Event mask: WL_EVENTING_MASK_LEN 16 (wlioctl_defs.h:1438), bit 69 = byte 8 bit 5; GET "event_msgs", set
  bit, SET.
- Design decision (small step): M4 runs inside the existing fw-up callback (PrepareHardware), after the M3
  ioctls, and ends with the same teardown (ARM halt, BME off, free). Making the firmware resident (device
  lifetime DMA, ReleaseHardware teardown) moves to M5, the NDIS prerequisite.
- dhd/fmac choose escan v2 when "scan_ver" answers (dhd wl_cfg80211.c:18501-18511, fmac feature.c:342 int get)
  -> driver queries scan_ver and builds v2 (fwil_types.h:409, fixed 72, version 2 :54/:74) or v1.
- Code (WlanMsg.c):
  - generic Ioctl(cmd, in, inlen, outlen) + IoctlVar(get/set);
  - H2D full check: no phase bit, same as fmac, so the ring may wrap;
  - IOCTL_CMPLT reposts its response buffer;
  - WL_EVENT: parse the bcm event and repost the buffer;
  - escan results deduped by BSSID (64 max), each logged as `[scan] BSSID ch rssi chanspec "ssid"`;
  - sequence: 8 event buffers, event_msgs get + set bit 69, UP(1), scan_ver, escan, poll up to 10 s;
    final line `SCAN n: ...`.
- Registry Parameters\Scan (default 1). MSG_DMA_SIZE 0x23000 -> 0x27000 (event buffers @0x23000).
  DriverVer 1.0.4.0. Build s10e-artifacts\wifi-m4, sys A9E67C40D5DF74AF.
- Review of f707b27:
  - Checked: event-buffer carve-out ends exactly at MSG_DMA_SIZE (0x23000 + 8 x 0x800 = 0x27000), so the DMA
    enabler MaximumLength follows automatically.
  - Checked: D2H XOR covers the 24 B item; EventPacket bounds `Len < 72` before any field read; the goto
    ScanEnd stays inside the block.
  - Found: EscanResult `Off + BLen > Len` wraps for a malformed BLen near 4 GB (reads stay bounded by the loop
    guard, but the check is wrong). Fixed in its own commit: `BLen > Len - Off`.
- Run m4-1 (02:37, 1.0.4.0): 8 event bufs posted (rxoffset 0); event_msgs GET (byte 8 = 0x21, ESCAN bit already
  set) + SET OK; UP OK; **WL_EVENT path works** (event type 54 status 0 datalen 5 parsed + buffer reposted);
  scan_ver answered -> v2 escan (86 B) -> IOCTL CMPLT status **-26 = BCME_EPERM** (bcmutils.h:385). SCAN 1.
  Teardown clean (ARM halted, BME off). Battery 64 %.
- Analysis: nvram ccode=GB regrev=0 (not a missing country). dhd (Samsung's driver for this fw) uses v2 only when
  wlc_ver major >= 14 (wl_cfg80211.h:833 MIN_ESCAN_PARAM_V2_FW_MAJOR) AND scan_ver answers
  (wl_cfg80211.c:18501-18511); fmac uses scan_ver alone. Hypothesis: this fw's wlc major < 14 -> expects v1.
- 1.0.4.1: GET wlc_ver (wl_wlc_version_t, 16 B, major @12), dhd rule, and on refusal one retry with the other
  version. Build s10e-artifacts\wifi-m4b, sys FA95DCDE….
- Run m4-2 (02:39) was void: I did not delete WINRUN.FLG before the reboot, so UEFI saved PRAM and went straight
  to TWRP ("previous boot ran Windows"). Every deploy must `rm -f /cache/WINRUN.FLG`.
- Run m4-3 (02:41, 1.0.4.1): wlc_ver epi 18.41.117.0 **wlc 12.1**; scan_ver answers; dhd rule -> v1 -> **-37
  BCME_VERSION**; retry v2 -> -26 EPERM again. So this fw wants v2 (fmac rule), and EPERM is a state issue.
  dhd wl_cfgscan.c:1327-1333 treats EPERM as "Escan not permitted at this point of time" (transient). Our
  escan comes milliseconds after WLC_UP.
- 1.0.4.2: v2 by scan_ver (fmac rule), escan retried on EPERM every 200 ms (events drained) up to 15 x. Note dhd
  sets v2 length = sizeof(wl_scan_params_v2_t) (wl_cfgscan.c:1043, includes channel_list[1] -> 76), fmac
  BRCMF_SCAN_PARAMS_V2_FIXED_SIZE 72; kept 72 (one change per run). Build wifi-m4c, sys 30267EB7….
- Run m4-4 (02:45, 1.0.4.2): escan v2 refused with EPERM on all 15 attempts over 3 s -> not transient.
- Cause candidate: no CLM (regulatory) blob. The vendor ships bcmdhd_clm.blob (29232 B, on the phone in
  drivers\bcm4375\); dhd applies it in preinit before UP (dhd_linux.c:10199 dhd_apply_default_clm); fmac does
  too (common.c:177-198 brcmf_c_process_clm_blob -> "clmload" chunks, :102-170).
- 1.0.4.3: WlanLoadBlob exported from WlanDl.c (was static LoadFile); ClmLoad before event_msgs SET + UP:
  - 1400 B chunks, header {flag = DL_BEGIN/DL_END | 1<<12, type 2, len, crc 0} (fwil_types.h:185-194,
    :1106-1112);
  - clmload_status logged on failure, clmver logged on success.
  IoctlVar now uses a 2008 B state buffer. Build wifi-m4d, sys 8340DDF7….
