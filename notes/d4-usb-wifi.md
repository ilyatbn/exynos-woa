# D4 — WiFi: USB adapter choice for the donation build

Plan-of-record task list: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D4.
Findings appended as discovered; facts carry source URLs; inference marked; dead
ends recorded.

## Closed already (do not re-open without new evidence)

- The S10e's own WLAN is Broadcom **bcmdhd** with no Windows driver and no DT
  device node — `notes/dock-desktop.md` §6, MILESTONES M12. Vendor WiFi on
  Windows is OUT.
- LAN-first is the network plan (M13): a USB ethernet dock device is inbox on
  Windows ARM64 (ASIX/Realtek RTL8153 class). D4 only covers the case where
  WiFi is genuinely wanted.
- Everything hangs off D1 host mode working (xHCI up, non-powered hub budget).

## Questions the research must answer

1. **Inbox**: does Windows 11 ARM64 ship ANY in-box USB WiFi driver? (Check the
   compatibility INF dumps for wlan class + USB PIDs; historically the inbox set
   is PCIe/NDIS-centric.)
2. **Vendor ARM64 drivers**: which USB WiFi chip vendors ship a signed ARM64
   Windows driver today — Realtek (RTL8812AU/RTL8822CU/RTL8192EU/RTL8188GU?),
   MediaTek (MT7601U/MT7610U/MT7612U?), Qualcomm/Atheros (USB AR9271 — last
   driver update?), Ralink RT3070? Look for "ARM64" or "Windows on ARM" in the
   driver packages, and where they are distributed (vendor site, Windows Update
   catalog).
3. **WoA community reports**: Renegade/Mu/Windows-on-Dragonboard threads — which
   specific adapters are confirmed working under Windows 11 ARM64 on Snapdragon
   devices (that is the same driver environment we will present).
4. **802.11 IHV wrinkle**: USB WiFi drivers are NIS + IHV-HAL miniports — any
   ARM64 signing/compat gotchas (e.g. drivers that work on x64 WoA emulation?
   no — kernel drivers never emulate; must be native ARM64).
5. **Recommendation**: name 1-2 concrete adapters (chip + a retail SKU) for the
   donation build, with the driver source URL and the evidence tier (inbox /
   vendor-signed / community-confirmed / unproven).

## Progress log

(append below)
