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
