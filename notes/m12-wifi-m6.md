# M12 WiFi milestone 6 — join a WPA2-PSK network with the firmware supplicant (no data path yet)

Goal: with the firmware resident (M5), join the network named in Parameters\Ssid using WPA2-PSK, the 4-way
handshake done by the firmware's own supplicant ("sup_wpa"), and log the association events. Proves auth +
assoc + key exchange. Dormant unless Ssid is configured, so it is safe to ship before the credentials exist.
Needs the user: SSID + passphrase of their network (never guessed, never committed).

## Protocol facts (fmac = brcmfmac master cfg80211.c / fwil.h; dhd = bcmdhd lineage-23.2)
- Order (fmac brcmf_cfg80211_connect, cfg80211.c:2440-2620):
  1. INFRA 1 (BRCMF_C_SET_INFRA 20, fwil.h:20; dhd wlioctl_defs.h:622; fmac :1423/:5255).
  2. "wpa_auth" = WPA2_AUTH_PSK 0x80 (dhd wlioctl_defs.h:555; fmac :1981/:2246).
  3. "auth" = 0 open (dhd wlioctl_defs.h:999 WL_AUTH_OPEN_SYSTEM; fmac :2020).
  4. "wsec" = AES_ENABLED 4 (dhd wlioctl_defs.h:489; fmac :2094).
  5. "sup_wpa" = 1, the firmware supplicant (fmac :2507, FEAT_FWSUP path).
  6. PMK: BRCMF_C_SET_WSEC_PMK 268 (fwil.h:82; dhd wlioctl_defs.h:875).
  7. SET_SSID 26 (fwil.h:25; dhd :628) with join params {SSID_len u32, SSID[32]} (fmac :2600-2617; the
     optional bssid/chanspec part is left out = any BSS with that SSID).
- PMK format (dhd wl_cfg80211.c:7100-7127, the vendor driver for this firmware):
  - The 32-byte PMK is sent as 64 lowercase hex chars, key_len 64, flags WSEC_PASSPHRASE 1
    (wlioctl.h:1543).
  - Struct wsec_pmk_t (wlioctl.h:1548-1552) = key_len u16, flags u16, key[WSEC_MAX_PASSPHRASE_LEN 256]
    (:1540), 260 B. fmac's copy has key[128] (fwil_types.h:584); dhd's size is used, since dhd matches this
    firmware generation.
  - The PMK is derived on the host: PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32) (IEEE 802.11i).
    Kernel CNG BCryptDeriveKeyPBKDF2 (ksecdd.lib), so nothing depends on firmware PBKDF2 support.
  - Passphrase 8..63 chars (wlioctl.h:1536-1538 WSEC_MIN/MAX_PSK_LEN), or Parameters\Psk = 64 hex chars
    used as is.
- Events (dhd bcmevent.h): SET_SSID 0 (:117, status 0 = success :322), AUTH 3, DEAUTH_IND 6, ASSOC 7,
  DISASSOC_IND 12, LINK 16 (:133, flags & 1 WLC_EVENT_MSG_LINK = up :55), PSK_SUP 46 (:162, handshake
  result). All set in event_msgs before the join.

## Progress log
- 2026-10-12 03:15: research above. User away; code is built dormant (no Ssid). Device test waits for the user.
- Link-up rule with the firmware supplicant (fmac brcmf_is_linkup cfg80211.c:6120-6146): SET_SSID status 0
  (assoc OK) AND PSK_SUP status 6 = BRCMF_E_STATUS_FWSUP_COMPLETED (fweh.h:138) = WLC_SUP_KEYED (dhd
  wlioctl.h:1491-1497 enum: DISCONNECTED 0 .. KEYXCHANGE 5, KEYED 6, TIMEOUT 7). PSK_SUP with any other status
  = handshake failure (:6184-6189).
- Code 1.0.6.0 (WlanMsg.c, WlanProbe.c, ksecdd.lib):
  - WlanJoinPrepare: Ssid 1..32 printable ASCII; Psk = 64 hex chars, or Passphrase 8..63 -> kernel CNG
    BCryptDeriveKeyPBKDF2(HMAC-SHA1, SSID, 4096, 32); passphrase buffers wiped.
  - WlanJoinSelfTest: 802.11i H.4.1 vector ("password"/"IEEE" -> f42c6fc5…9710a12e, cross-checked on the PC
    with Python hashlib), logged every start.
  - WlanMsgJoin:
    - event_msgs += SET_SSID/AUTH/DEAUTH_IND/ASSOC/DISASSOC_IND/LINK/PSK_SUP; INFRA 1, wpa_auth 0x80, auth 0,
      wsec 4, sup_wpa 1;
    - SET_WSEC_PMK 260 B (64 hex, flags 1), then the PMK copy, the DMA request buffer and VarBuf are wiped;
    - SET_SSID 36 B; poll up to 20 s.
    - Result JOIN_OK = SET_SSID 0 && PSK_SUP 6; JOIN_FW_FAILED on SET_SSID != 0, another PSK_SUP status, or
      deauth/disassoc/link-down.
  - Runs once after the t = 30 s rescan when Ctx->Join.Valid. Build wifi-m6, sys 11A3BC9F….
- To test (user): `reg add HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Wlan\Parameters /v Ssid /t REG_SZ
  /d "<ssid>"` and `/v Passphrase /t REG_SZ /d "<passphrase>"`, reboot, read Exynos9820Wlan-run.txt
  (`JOIN 0` = associated + keyed). The passphrase sits in the registry in plain text: dev bring-up only.
- Run m6-1 (03:14, 1.0.6.0): **PBKDF2 self-test PASS** (kernel CNG). The join is dormant (no Ssid).
  PrepareHardware SCAN 0 (14 BSS) + RESIDENT; the t = 30 s rescan gave SCAN 0, 9 BSS.
  - Again one "not synced" item whose log shows fw w == our position (42), as in m5-3. The loop only runs
    while W != position, so a second consumer is the explanation: the timer re-enqueues the work item every
    100 ms while the 2 s rescan runs inside it, and a second instance drains the same ring concurrently.
  - 1.0.6.1: an interlocked PollBusy flag means only one poller runs at a time. Build wifi-m6b.
- Run m6-2 (03:18, 1.0.6.1): self-test PASS, SCAN 0 (11 BSS), RESIDENT, t = 30 s rescan SCAN 0 (10 BSS, 57
  events total) with **no unsynced item**. The concurrent-poller diagnosis holds.
- Review of 5e7b44a: no return between the flag acquire and its release in WlanPollWork (both early returns
  come before the acquire). D0Exit / ReleaseHardware stop the timer and flush, so the flag cannot be left
  set across teardown. No findings.
- State: M6 code is complete and dormant. **Waiting for the user's SSID + passphrase** (registry, see above).
  The join is the only M6 step still to validate.
- Run m6-3 (2026-10-12, user set Ssid/Passphrase, 1.0.6.1): t = 30 s rescan SCAN 0 (8 BSS, target "HomeX-5"
  seen at 44:DF:65:D7:CD:41 ch 36 rssi -66). Join: event_msgs get/set, INFRA, wpa_auth, auth, wsec all status 0;
  **"sup_wpa" = 1 -> CMPLT status -23 = BCME_UNSUPPORTED** (bcmdhd include/bcmutils.h:382). JOIN 1 (setup
  failed), nothing sent to the AP; the firmware stayed alive (t = 180 s).
  - Meaning: this firmware build (bcmdhd_sta.bin 18.41.117) has **no in-dongle supplicant**. The vendor driver
    only uses sup_wpa under BCMSUP_4WAY_HANDSHAKE + dhd_use_idsup (dhd_linux.c:10670-10681); on Android the 4-way
    handshake is done by the host wpa_supplicant over EAPOL frames on the data path.
  - Consequence: WPA2 needs the **host-side 4-way handshake** (EAPOL rx/tx over the msgbuf data path = tx flowring
    + rx completion, PTK via PRF-SHA1, MIC HMAC-SHA1, GTK AES key unwrap, keys installed with WLC_SET_KEY). The
    data path is M7 work anyway, so M6 is re-scoped: association (SET_SSID status 0) + host handshake, done after
    the M7 data path exists. The PMK derivation (self-test PASS) is reused.
