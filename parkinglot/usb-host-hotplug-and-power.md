# Parked: USB host hot-plug + power budget (user, 2026-10-12)

State (boot #87): host mode works for devices attached BEFORE power-on - the fallback VBUS boost is decided once, in
UEFI (Beyond0LteUsbHostDxe), from CC_STATUS0 CCPinStat + BC_STATUS.VBUSDet. Dock lights up during the shim.

Open problems:
1. Hot-plug after boot: no power. Nothing watches the USB-C port at runtime (no CCIC/PD component in Windows),
   so a dock/device attached later never gets the boost. Needs a runtime component: the MAX77705 is on HSI2C45 -
   either a Windows driver for the CCIC (interrupt on its INTB GPIO) or an agent poll through S10eDiag + an I2C
   path, then boost on/off (charger CNFG_00 mode 0xE / 0x4) on attach/detach. Also detach must turn the boost OFF.
2. Keyboard sometimes dead even when attached before boot: suspected power (lit mechanical keyboard + dock hub +
   2.5 GbE; OTG limit 1.2 A since boot #86). Try 1.5 A (CNFG_02 bits 7:6 = 11), and/or a plain keyboard to split
   power from enumeration timing.
3. Real SuperSpeed: LC PLL never locks (PMA 0x350 = 0x3E/0x38) - reference clock / PMIC LDO10-12; USB_SS_LOCK_ATTEMPT 1
   to retry. Needed for 5 Gbps and the dock's HDMI (DP alt mode + CCIC PD/VDM).
4. Charging through a PD dock: needs runtime PD (PR_SWAP / DR_SWAP); do NOT plug a charger into the dock until then.
Refs: notes/m16-usb.md (runs 9-16), notes/m16-ss-phy.md, notes/m16-ccic-init.md, notes/m17-muic-switch.md.

## 2026-10-12: OTG boost stays on for the whole Windows session (battery finding)
Booted without a source attached, the UEFI host path leaves MAX77705 CNFG_00 in mode 0xE (boost, charger engine
off). Nothing in Windows switches it back, so a charger plugged in later (a) does not charge and (b) meets a
VBUS the phone is already driving. Linux handles this in max77705_charger.c on CCIC/MUIC attach (OTG off,
mode 0x4 + 1 ms, then CHG+BUCK). Needs the CC attach state in Windows (CCIC interrupt or a poll) before any
mode write. S10eBattery 1.0.0.3 only reports the state honestly (off line while boosting).
