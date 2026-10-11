# W02 — WiFi live reads: PCI identity, firmware blob names, IRQ wiring

Status: open — **needs-phone** (the S10e was not attached on 2026-10-11;
`adb devices` empty)

## Reads (all read-only)

```
adb shell 'ls -l /sys/bus/pci/devices/' 2>/dev/null
adb shell 'for d in /sys/bus/pci/devices/*; do echo $d; cat $d/vendor $d/device; done'
adb shell 'ls /vendor/firmware | grep -iE "bcmdhd|nvram|4375"'
adb shell 'cat /proc/interrupts | grep -i pcie'
adb shell 'ls /sys/devices/platform/13ed0000.pcie0/'
```

SELinux may block some of this from an unprivileged shell (known pattern);
TWRP boots the same reads without policy.

## What each closes

| Read | Open item |
|---|---|
| `device` under `14e4` | **P2** — confirm `0x4475` (or 4476/4477) → VERIFIED constant + INF hardware ID |
| `/vendor/firmware` names | **P3** — exact `bcmdhd_sta*.bin` / `nvram_net.txt` variants for the installer copy stage and `WLAN_FW_FILE`/`WLAN_NVRAM_FILE` |
| `/proc/interrupts` pcie line | confirms SPI 253 wiring end-to-end (our GSIV 285 derivation) |
| `13ed0000.pcie0/` attrs | link state, L1SS knobs — free extra facts |

## Also worth grabbing while the phone is up

- `ls /sys/bus/pci/devices/*/resource` — the BAR0 address Android assigned
  (cross-check against the DT `ranges` 0x14000000 window).

## Update 2026-10-12 (M12 milestone 1, notes/m12-wifi-m1.md)

- P3 partly closed offline from the stock G970FXXSGHWC1 vendor image: `/vendor/firmware/bcmdhd_sta.bin_b1`,
  `bcmdhd_clm.blob`, `nvram.txt_1rh_es43_b1` ("1rh", Murata by inference) / `nvram.txt_CS01_semco_b1` (SEMCO) + B0 variants.
  Still needs-phone: the phone's own vendor listing + hashes, and which module (Murata/SEMCO) this unit has.
  Exact pull commands: notes/m12-wifi-m1.md "Firmware blobs for milestone 2".
- P2 now also closes from our own UEFI print (`[PCIe] R1: endpoint 14e4:XXXX`) - no Android needed.
- With userdata = Windows NTFS and no Android boot, the /sys and /proc reads above are only possible
  after a rollback to Android; they are no longer required for milestone 1.
