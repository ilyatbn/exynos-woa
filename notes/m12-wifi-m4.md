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
