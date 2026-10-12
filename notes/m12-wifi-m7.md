# M12 WiFi milestone 7 (+ M8 adapter/app) — msgbuf data path, host WPA2 4-way handshake, NetAdapterCx, wifi-connect

Tasks (one commit each, agent worktree branch; device runs by the main session):
- M7a msgbuf data path: tx flowring create, txpost, tx completion, rxpost + rx completion, pktid tables, cache maint.
- M7b host WPA2-PSK 4-way handshake (EAPOL over the data path), PRF/PTK/MIC/key-unwrap via CNG, WLC_SET_KEY.
- M7c NetAdapterCx client (Class=Net), link state from the handshake, tx/rx queues on the data path.
- M8 tools/wifi-connect GUI (Ssid/Passphrase registry + device restart + run-log tail).

Sources: dhd = vendor bcmdhd (scratchpad src/bcmdhd: dhd_msgbuf.c, include/bcmmsgbuf.h, wlioctl.h,
wlioctl_defs.h, 802.11.h, ethernet.h), fmac = brcmfmac master (src/brcmfmac: msgbuf.c, commonring.c,
fwil_types.h). Line numbers are of those copies.

## Progress log
- 2026-10-12: worktree fast-forwarded a8d8a62 -> main 8efa319 (it was created from an old commit; the WLAN
  driver did not exist on it). Read m5/m6 notes, design doc, WlanMsg.c / WlanProbe.c / headers.

### Protocol facts — M7a data path (dhd include/bcmmsgbuf.h; fmac msgbuf.c)
- Msg types (bcmmsgbuf.h:160-181): FLOW_RING_CREATE 0x03, FLOW_RING_CREATE_CMPLT 0x04, TX_POST 0x0F,
  TX_STATUS 0x10, RXBUF_POST 0x11, RX_CMPLT 0x12.
- tx_flowring_create_request_t (bcmmsgbuf.h:439-455 == fmac msgbuf.c:181-194), 40 B on the H2D ctrl ring:
  hdr 0..7, da[6] 8, sa[6] 14, tid 20, if_flags 21, flow_ring_id u16 22, tc 24, priority_ifrmmask 25,
  int_vector u16 26, max_items u16 28, len_item u16 30, flow_ring_ptr (addr64) 32.
- Completion ring_create_response_t (:758-764): hdr, compl_msg_hdr {status s16 @8, flow_ring_id u16 @10}.
- Ring ids (dhd_msgbuf.c:210-273): flowid 2 = first flowring = ringid 5; create request carries the
  flowid (dhd_msgbuf.c:9334 flow_ring_id = flowid; fmac :650 flowid + BRCMF_H2D_MSGRING_FLOWRING_IDSTART 2).
- Flowring TCM index slots (dhd_pcie.c:8889-8932): after the 2 H2D common rings the H2D w/r index arrays
  continue for the flowrings -> flowid F's slots = h2d_w_idx_ptr + RW_INDEX_SZ * F (same for r).
- Flowring geometry: max_items = h2d_max_txpost = H2DRING_TXPOST_MAX_ITEM 512 (dhd_msgbuf.c:438, :9340;
  fmac msgbuf.h:15), len_item = H2DRING_TXPOST_ITEMSIZE 48 (bcmmsgbuf.h:47; fmac msgbuf.h:24). Memory is
  host-allocated, address in the request (dhd :9338, fmac :654).
- host_txbuf_post_t (bcmmsgbuf.h:966-997), 48 B: hdr 0..7 (request_id = pktid), txhdr[14] 8 (the 802.3
  header), flags 22 (BCMPCIE_PKT_FLAGS_FRAME_802_3 0x01 | prio << 5, :999/:1019), seg_cnt 23 (1),
  metadata_buf_addr 24, data_buf_addr 32 (payload AFTER the 14 B header), metadata_buf_len u16 40,
  data_len u16 42 (payload length = frame - 14, fmac :763), ext/marker 44. fmac uses request_id = pktid + 1
  (msgbuf.c:752, :892) so 0 is never a pktid.
- host_txbuf_cmpl_t (:1046-1080), 24 B: hdr (request_id = pktid), status s16 @8, flowring id @10,
  metadata_len @12, tx_status u16 @14.
- host_rxbuf_post_t (:905-918), 32 B: hdr (request_id = pktid), metadata_buf_len u16 8, data_buf_len u16 10,
  rsvd 12, metadata_buf_addr 16, data_buf_addr 24. Buffers 2048 B (fmac BRCMF_MSGBUF_MAX_PKT_SIZE msgbuf.c:56,
  dhd DHD_FLOWRING_RX_BUFPOST_PKTSZ); no metadata (fmac rx_metadata_offset 0).
- host_rxbuf_cmpl_t (:926-959), 40 B: hdr (request_id = pktid, if_id = ifidx), status @8, ring id @10,
  metadata_len @12, data_len u16 @14, data_offset u16 @16, flags u16 @18 (FRAME_802_3 vs 802_11 in bits 0-2,
  fmac :1210), rx_status_0/1 @20/@24. Data starts at data_offset, or rx_dataoffset (shared dma_rxoffset, 0
  on this fw per m4-1) when data_offset == 0 (fmac :1199-1203, dhd same).
- XORCSUM sync applies to every D2H ring item (dhd d2h_sync_cb on rx cpl dhd_msgbuf.c:5592, and generic
  :5992), so D2hItemReady + epoch is reused for the tx/rx completion rings.
- max rx buffers: shared max_host_rxbufs (fmac pcie.c:1651, default BRCMF_DEF_MAX_RXBUFPOST 255 when 0).

### Protocol facts — M7b handshake / keys
- Firmware has no supplicant (m6-3: sup_wpa -> -23). dhd host-supplicant path: set "wpaie" = the RSN IE the
  supplicant put in the assoc request (wl_cfg80211.c:6230-6257; fmac cfg80211.c:2420-2434), then SET_SSID.
  After assoc, dhd reads "assoc_info" (wl_assoc_info_t req_len/resp_len/flags, wlioctl.h:1663-1671) and
  "assoc_req_ies" (the IEs only, wl_cfg80211.c:15040-15077) - we use that to get the RSN IE actually sent.
- Keys: iovar "wsec_key" with wl_wsec_key_t (dhd wl_cfg80211.c:6906 / fmac cfg80211.c:558; the iovar form
  of WLC_SET_KEY 45, fwil.h:36). wl_wsec_key_t (wlioctl.h:1513-1533), 164 B: index u32 0, len u32 4,
  data[32] 8, pad_1[18] 40, algo u32 112, flags u32 116, pad_2[2] 120, pad_3 128, iv_initialized 132,
  pad_4 136, rxiv.hi u32 140, rxiv.lo u16 144, PAD 146, pad_5[2] 148, ea[6] 156, PAD 162.
  - Pairwise (wl_add_keyext :6853-6907): index 0, ea = AP MAC, flags 0, algo CRYPTO_ALGO_AES_CCM 4
    (wlioctl_defs.h:420), len 16 (TK).
  - Group (wl_cfg80211_add_key :7065-7074): index = GTK key id, ea 0, flags WL_PRIMARY_KEY 2
    (wlioctl_defs.h:473), algo 4, rxiv from the msg-3 Key RSC (hi = rsc[5..2], lo = rsc[1..0],
    iv_initialized 1; :6886-6895).
  - "need to guarantee EAPOL 4/4 send out before set key" (dhd_wait_pend8021x, :6903-6904): wait for msg 4's
    TX_STATUS before installing the pairwise key.
- No "authorize" ioctl in the STA host-supplicant path (fmac/dhd add_key only), so none is sent.
- PRF self-test vectors (IEEE 802.11i-2004 H.3 cases 1-2), recomputed on the PC with Python hmac/hashlib
  (scratchpad py/prf.py), PRF(K, A, B, 64):
  - K = 0x0b x 20, A "prefix", B "Hi There" -> bcd4c650b30b9684951829e0d75f9d54b862175e...8356cf5a
  - K "Jefe", A "prefix", B "what do ya want for nothing?" -> 51f4de5b33f249adf81aeb713a3c20f4...9092b8fc
  (both equal the standard's published values as I recall them; my recollection of case 3 did NOT match
  the computation, so case 3 is not used.)
- AES key wrap: RFC 3394 4.1 (KEK 000102..0F, data 00112233..EEFF -> 1FA68B0A8112B447 AEF34BD8FB5A7B82
  9D3E862371D2CFE5), recomputed with Python cryptography aes_key_wrap/unwrap.

### NetAdapterCx version (M7c) - checked 2026-10-12
- Installed WDK 10.0.26100.0 ships KMDF NetAdapterCx headers/stub libs 2.0..2.5
  (Include\10.0.26100.0\km\netcx\kmdf\adapter\2.x, Lib\...\km\arm64\netcx\kmdf\adapter\2.x\netadaptercxstub.lib);
  msbuild default for KMDF is 2.4 (build\10.0.26100.0\WindowsDriver.KernelMode.CX.Default.props:29-30),
  selected with <NetAdapterDriver>true</NetAdapterDriver> + NETADAPTER_VERSION_MINOR.
- Microsoft "NetAdapterCx version overview" (learn.microsoft.com, fetched 2026-10-12): Win11 24H2 = 2.5,
  Server 2022 23H2 = 2.4, **Win11 22H2 = 2.3**, 21H2 = 2.2, Win10 2004 = 2.0 (KMDF 1.33 from 21H2 on).
- The phone's image is Windows 11 23H2 22631.2861 ARM64 (notes/m10-windows-deploy-plan.md:43), which is the
  22H2 servicing branch (22621 base) -> **target NetAdapterCx 2.3** (2.4 = WDK default would not bind on
  22631). UNVERIFIED on the device: the main session should confirm `ver` = 22631 before installing M7c.

## M7a code (1.0.7.0)
- WlanMsg.c:
  - H2dNext/H2dCommit generalise the ctrl-ring writer to any H2D ring (rxpost, flowring).
  - D2hPoll(ring) drains any completion ring; new cases FLOW_RING_CREATE_CMPLT, TX_STATUS, RX_CMPLT.
    D2hPollAll = rx, tx, ctrl (fmac rx_trigger order). WlanMsgPoll now drains all three.
  - Per-ring MDLs replace the single MdlD2h (a ctrl poll no longer invalidates 48 KB of tx/rx rings); the
    "not synced" bookkeeping moved into MSG_RING (the log line now names the ring).
  - DataInit at the resident hand-over: 2nd common buffer 0x66000 B (flowring 512x48, 64 tx + 128 rx 2 KB
    buffers, one MDL each), zeroed + cleaned, rx buffers posted (min(128, max_host_rxbufs)). Logged as
    "DATA 0: ..." in the PrepareHardware report (Exynos9820Wlan.txt).
  - FlowRingCreate(Da): flowid 2, tid 0, SA = cur_etheraddr, DA = AP, TCM w index 0 first; waits 2 s for the
    completion; "FLOW 0/1" line. Not called yet (M7b calls it after association).
  - TxFrame: 14 B header in the item, payload in a tx buffer (clean), flowring item (clean), w index + DB0.
    RxComplete: invalidate the buffer, EAPOL (0x888E) copied for the handshake, other frames -> RxFn hook;
    buffer reposted, rxpost committed once per drain.
  - API: WlanMsgTx, WlanMsgSetRx. WLAN_REPORT_MAX 16384 -> 32766 (more log lines per run).
- Build s10e-artifacts\wifi-m7a, sys BC8CBEE32177E076F6C4D19CE88DC3F8C2BF6E138D975F9E002F44F1496505DD
  (W4/WX clean, first try).
- Not device-tested on its own: the first observable is "DATA 0" + the rxpost fw r index; flowring/tx/rx are
  exercised by M7b's join, so the device test is planned with the M7b build.
- Review of e326ee6 (adversarial):
  - Checked: txpost/rxpost field writes vs dhd_prot_txdata (dhd_msgbuf.c:6750-6890: flags 802_3 | prio<<5,
    seg_cnt 1, data_len = len - 14, metadata 0) and dhd_prot_rxbuf_post (:4605-4620: epoch = seqnum % 253,
    data_buf_len, metadata 0). dhd does not set the epoch on txpost items; ours carries seq % 253 - harmless
    (same as the ctrl ring, which the fw accepts), noted rather than changed.
  - Checked: per-ring MDL sizes == ring geometry (64x24, 1024x24, 512x40); RegionMdlsFree frees them; old
    MdlD2h has no remaining user (rg). WlanMsgFree deletes DataBuffer while quiesced only.
  - FOUND: RxComplete set RxCount = I when a repost failed - every later completion of a higher buffer would
    then be "unknown" and never reposted. Fixed (follow-up commit): count RxLost, keep RxCount.
  - FOUND (cosmetic): magic 0x02 for the 802.11 frame flag -> PKT_FLAGS_FRAME_802_11 (bcmmsgbuf.h:1000).
  - Rebuilt wifi-m7a: sys 2FF9300287AE6C9F85600B492E3BFACE645527418D0473801636C78D3B95CECE.

### Protocol facts - M7b EAPOL-Key (hostap = wpa_supplicant master, downloaded 2026-10-12 to scratchpad src/hostap)
- EAPOL header (802.1X): version, type 3 = EAPOL-Key, length BE16. wpa_supplicant sends version =
  conf eapol_version (default 1).
- struct wpa_eapol_key (wpa_common.h:246-257): type (2 = RSN), key_info BE16, key_length BE16, replay[8],
  nonce[32], iv[16], rsc[8], id[8], MIC[16] (HMAC-SHA1 AKM), key data length BE16, key data. Offsets from the
  EAPOL header: 4 type, 5 info, 7 len, 9 replay, 17 nonce, 49 iv, 65 rsc, 73 id, 81 MIC, 97 data len, 99 data.
- key_info bits (wpa_common.h:226-242): version mask 0x7 (2 = HMAC_SHA1_AES), KEY_TYPE 0x8 (pairwise),
  INSTALL 0x40, ACK 0x80, MIC 0x100, SECURE 0x200, ERROR 0x400, REQUEST 0x800, ENCR_KEY_DATA 0x1000.
- msg 2 (wpa.c:550-700): key_info = ver + KEY_TYPE + MIC; key_length 0 (RSN); replay = msg 1's; nonce = SNonce;
  key data = the RSN IE of the assoc request. msg 4 (wpa.c:2416-2490): key_info = (msg3 AND SECURE) + ver +
  KEY_TYPE + MIC; replay = msg 3's; no key data.
- MIC (wpa_common.c:458-482): HMAC-SHA1(KCK, whole EAPOL frame with MIC zeroed), first 16 B.
- PTK (wpa_common.c:597-650): PRF(PMK, "Pairwise key expansion", min(AA,SPA), max(AA,SPA), min(ANonce,SNonce),
  max(ANonce,SNonce)); CCMP: KCK 16, KEK 16, TK 16 = 48 B.
- Key data of msg 3 is AES-key-wrapped with KEK (ENCR_KEY_DATA); KDE = DD len 00-0F-AC type data; GTK KDE type 1
  (RSN_KEY_DATA_GROUPKEY wpa_common.h:144): keyid bits 0-1, tx bit 2, reserved, GTK (wpa.c:1689-1702);
  padding = DD followed by 0 (wpa_common.c:4388).
- RSN IE: tag 48 (802.11.h:1450), version 1 (wpa_common.h:68), CCMP 00-0F-AC-04 (:124), PSK AKM 00-0F-AC-02
  (:84); capabilities MFPR = bit 6, MFPC = bit 7 (wpa_common.h:194-195).
- wl_bss_info_v109 (wlioctl.h:384-424): ie_offset u16 @116, ie_length u32 @120.
- 2026-10-12 (main session took M7b over after the subagent stopped): WlanMsgJoin rewritten for a HOST
  supplicant: SupSelfTest; rescan with TargetSsid (BSSID + RSN IE of the strongest match); own RSN IE (v1, the
  AP's group cipher, CCMP, PSK, caps 0; refuses MFPR) set with "wpaie"; INFRA/wpa_auth/auth/wsec (no sup_wpa);
  SET_SSID; the tx flowring (DA = AP) is created once SET_SSID reports status 0; SupEapol handles msg 1/4
  (SNonce from BCryptGenRandom, PTK, msg 2 with our RSN IE), msg 3/4 (ANonce check, MIC verify, GTK KDE from the
  AES-unwrapped key data, msg 4, wait for its tx completion, pairwise key idx 0 ea = AA, GTK with RSC, flags
  WL_PRIMARY_KEY - dhd wl_add_keyext / add_key), group rekey 1/2 (also from WlanMsgPoll). Replay counter checked.
  Result JOIN_OK only after the keys are installed; JOIN 5 = handshake failed (bad MIC on msg 3 = wrong
  passphrase). Driver 1.0.7.1, package s10e-artifacts\wifi-m7b, sys sha256 87acacf9e30afea5….
- **Run w7b (1.0.7.1, boot #99):** self-test PASS; target 44:DF:65:D7:CD:41 rssi -69, AP RSN IE 22 B; wpaie,
  INFRA, wpa_auth, auth, wsec all status 0; SET_SSID -> events AUTH 0, LINK up, ASSOC 0, SET_SSID 0 =
  **associated**; FLOW_RING_CREATE status 0 (flowring 2 open in 10 ms). Then **no EAPOL frame reached the host**
  (eapol rx 0, no [rx] lines at all) and 2.7 s later DEAUTH_IND reason 15 (4-way handshake timeout). => the rx
  data path delivers nothing (or msg 1 is dropped before RxComplete).
- 1.0.7.2 (wifi-m7b2): diagnostics only - at JOIN end the rxpost/rx-cpl/tx-cpl ring indices, rx/tx counters and
  the firmware console dump.
- **Run with 1.0.7.2 (boot #101, audB3 cycle):** same as w7b (associated, flowring open in 10 ms, DEAUTH reason
  15 after 2.7 s). Ring indices at JOIN end: **rxpost host w 128 / fw r 128** (firmware took every rx buffer),
  **rx cpl fw w 0** (never completed one), tx cpl w 0, rx frames 0. Firmware console: only "EL: 30 6c50" once a
  second. => the firmware holds received frames; something in our data-path setup differs from DHD. Audit
  delegated (notes/m12-wifi-m7-rx-audit.md).
- **Run with 1.0.7.3 (2026-10-12 evening, wifi-m7b3, log scratchpad runs/wifi-rx2/last/wifi/Exynos9820Wlan.txt):**
  fw console at boot: "allocating a max of 511 rxcplid buffers", "SPLITRX_MODE: 4 tcmsegsize 0 d11rxoffset 0";
  shared max_host_rxbufs 511, dma_rxoffset 0; DATA 0 posted 128 (fw r 0 at that moment). DOWN 0 / UP 0 bounce
  before the join -> rxpost host w 128 fw r 128; rxbufpost_sz = **1670**; SET_SSID -> AUTH/LINK/ASSOC/SET_SSID 0,
  flowring open in 10 ms, DEAUTH_IND reason 15 after 2680 ms; JOIN end: rx cpl fw w 0, rx frames 0, console empty.
  => audit fix 1 as an A/B bounce did NOT help.

### M7c rx-path work (1.0.7.4, package s10e-artifacts\wifi-m7c) - 2026-10-12, no device access in this session
Refs: scratchpad src/bcmdhd (LineageOS lineage-23.2) and src/brcmfmac (torvalds master), as in the audit.
- fmac attach order (msgbuf.c:1650-1665): rxbuf_data_fill (all max_rxbufpost, up to 10 x 10 ms retries) ->
  event bufs -> ioctlresp bufs; hostready only later from brcmf_pcie_preinit (pcie.c:1460-1468, also enables
  the mailbox interrupt mask first). DHD dhd_sync_with_dongle (dhd_msgbuf.c:4199-4220): GET rxbufpost_sz (u16,
  > DHD_FLOWRING_RX_BUFPOST_PKTSZ_MAX -> default) -> dhd_msgbuf_rxbuf_post -> dhd_preinit_ioctls.
- fmac refill: rxbufpost -= 1 per RX_CMPLT, refill to max when rxbufpost <= max - 32
  (BRCMF_MSGBUF_RXBUFPOST_THRESHOLD msgbuf.c:58, :1029-1035). DHD bursts of 32 (RX_BUF_BURST dhd_msgbuf.c:102,
  :4438-4475). dhd data_buf_len = PKTLEN of a rxbufpost_sz packet (dhd_msgbuf.c:4493, :4521, :4607).
- fmac D2H mailbox (pcie.c:800-831): read shared d2h_mb_data (shared+44, pcie.c:232), write 0, DS_ENTER_REQ 0x2 ->
  send H2D_HOST_DS_ACK 0x2 (pcie.c:259-266, :763-795: h2d_mb_data word + config SBMBX write). PCIMailBoxInt rev>=64
  0xC30 (pcie.c:147).
- Task 1 (reorder, 1.0.7.4): DataInit moved from the resident hand-over to WlanMsgRun step 5b (right after the
  8 ioctl-resp buffers, before cur_etheraddr / CLM / UP); the Stop path now deletes M->DataBuffer after ARM halt +
  BME off (WdfObjectDelete after the existing halt sequence); the 1.0.7.3 DOWN/UP bounce removed (join logs
  "rxpost host w/fw r before SET_SSID" instead). Build W4/WX clean (build-package.ps1 into scratchpad m7c-build).
- Review of c98abe5 (adversarial): checked FwUp is called exactly once per WlanDlRun (WlanDl.c:943) and Msg is
  zeroed before it (WlanProbe.c:432), so M->DataBuffer cannot be stale on the Stop path; DataInit only needs
  step 1/2 state (MaxFlowrings, H2d bases, MdlAll) - all set before 5b; Stop order = halt, BME off, delete
  control buffer, delete data buffer, free MDLs (same lifetime rule as the control buffer); no remaining WLC_DOWN
  user (header define kept, cited). FOUND: the comment cited dhd_msgbuf.c:4217-4220 for "rx post ahead of preinit"
  - the actual lines are 4219 (rxbuf_post) vs 4224 (preinit). Fixed in a follow-up commit.
