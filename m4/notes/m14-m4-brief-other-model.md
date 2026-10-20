# M4 big cores - brief for another model (2026-10-15)

Paste the block below into a new Claude Code session opened in `C:\Users\ilya\code\exynos-woa` with the phone
attached. Before pasting, the user decides and adds:

- whether the phone's SSH firewall rule may be opened to the home network
  (`drivers/S10eDiag/esp/extra-ssh-install.cmd`, line `remoteip=192.168.137.0/24`; that change was blocked last time);
- whether changing the Windows kernel's code in memory at boot is allowed for experiments (one such change, the
  hardware access-flag test, was already done and worked).

```
Goal: get all 8 cores of the Galaxy S10e (Exynos 9820) running in Windows. The 2 Exynos M4
big cores (MADT gicc6/7) hang Windows whenever Windows runs work on them. Work unattended; the
phone is attached (adb at C:\Users\ilya\code\platform-tools\adb.exe). Follow AGENTS.md.
Never write MAC or IP addresses into files or commits.

Read first, in this order:
1. notes/m14-m4-summary.md  (one-page state: established facts, ruled out, tools, pitfalls)
2. notes/m14-smp.md, everything from "## PC sampler" to the end (2026-10-15 runs)
3. docs/exynos9820-port/HANDOFF.md sections 42 and 43

Established on 2026-10-15, do not redo:
- CoreSight/EL3 view is closed: DBGAUTHSTATUS 0xAA on every core (all external debug off).
  Samsung's s2d/arraydump areas are empty. At the death every core is still powered, not halted.
- Hardware access flag is NOT the cause: firmware switch BEYOND0LTE_NT_NO_HWAF (BootLogDxe)
  makes Windows run with TCR.HA=0 on every core (verified). Windows is fine on 6 cores that
  way, but 8 cores still die.
- Verbose KD print masks add nothing useful.
- A busy, CACHED M4 is harmless: park-stub flag 6 (BEYOND0LTE_PARK_STRESS_MASK) ran both M4s
  in a cached copy loop with DC CIVAC / IC IALLUIS / TLBI VMALLE1IS for 16+ min while Windows
  ran healthy on the 6 small cores (~1.2 M passes each).
=> The trigger needs Windows running ON the M4.

Next task: park-stub flag 7 = flag-6 stress PLUS interrupts on the M4:
- extend the flag-3/6 identity map with a Device-nGnRnE 1 GiB block for 0x0-0x3FFFFFFF
  (GIC is there), MAIR attr1 = 0x00
- the M4 enables its own banked GIC CPU interface (GICC 0x10102000: CTLR=1, PMR=0xF0) and its
  banked PPI 30 in the GICD (0x10101000: ISENABLER0 bit 30, IPRIORITYR)
- EL1 physical timer (CNTP_TVAL/CTL_EL0), 26 MHz, ~1 ms period; GSIV 30 is verified from the
  stock DT
- VBAR_EL1 = the mailbox page, with branches to an IRQ handler in the stub, the same way the
  PC sampler's fault vectors are installed (ParkPcSampleSetup). The handler reads IAR, writes
  EOIR, re-arms the timer, counts IRQs at a mailbox offset, and uses only x27-x29.
- loop: one copy pass, then WFI with IRQs unmasked (Windows' idle is WFI + interrupts; every
  earlier stub WFI test had interrupts masked)
- UEFI log line: IRQ count and passes moving
Test: PARK 0xFE, OFFER 0x3E, flag 7 on both M4s, Windows on the 6 small cores, stay.flg on,
soak 15 min. If the SoC dies, interrupts/WFI on the M4 are the trigger: then bisect timer vs
WFI vs SGIs. If it survives, next try EL0 round trips (eret to EL0 + svc) and Windows' MMU
attributes (4-level tables, 16-bit ASIDs, MAIR 0x444400FF444400FF, IPS 48-bit).

Tools and recipes:
- Build: from firmware/, powershell -NoProfile -ExecutionPolicy Bypass -File
  tools/build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG, then
  tools/pack-uefi-beyond0lte.ps1 -Ref ../../s10e-artifacts/ref-recovery.img
  -Output ../../s10e-artifacts/boot-<name>.img. Test switches are sed'd in before the build
  and restored with git checkout -- Platform afterwards. Always also set
  BEYOND0LTE_GPU_POWER_ON 1 and BEYOND0LTE_USB_KD_DEVICE 1 (BCD debug is on). Keep
  BEYOND0LTE_WINDOWS_WATCHDOG at its default 1.
- Flash from TWRP: adb push img /tmp/boot.img; dd to /dev/block/by-name/boot; check the
  readback (dd bs=4096 count=4457 | sha256sum). rm /cache/WINRUN.FLG before every test reboot.
- Safeguard (committed, verified): with stay.flg on, the 2nd Windows death in a row goes to
  TWRP by itself.
- KD: firmware/tools/kdrun/kdrun.exe <log> <total_s> <break_s> go "<init-cmds>" "<per-break-cmds>".
  Passive when break_s is huge. Read the M4 mailboxes with !dq [uc] ba245xxx / ba246xxx
  (gicc6/7 pages). Only one kdrun at a time.
- Phone monitor (user rule, always running): tools/phone/watchall.sh as a Monitor, re-armed
  when it expires.
- Getting to TWRP from Windows: kdrun <log> 120 8 twrp.
- Battery: check on each boot; below 15% park in TWRP for 30 min.

Phone state when you start: the parked state in HANDOFF section 43 (boot-kd6.img, 6 cores,
stay.flg on). The phone's SSH from this PC is blocked by a hotspot-only firewall rule.
```

The build helper used on 2026-10-15 (`mkimg.sh`, which seds the switches, builds, packs and restores) lived only in a
scratch directory; the recipe above is enough to recreate it.
