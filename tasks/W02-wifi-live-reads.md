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
