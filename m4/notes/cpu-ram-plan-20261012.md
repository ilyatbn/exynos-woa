# CPU (8 cores) + missing RAM - state of play and ranked plan (2026-10-12)

Read-only research task (no device, no builds). Every claim cites file:line.

## Progress log

- P01 created file; MILESTONES rows read: topology row says 6+2 MPIDR clusters (`docs/exynos9820-port/MILESTONES.md:224`), not 2+2+4; "Give Windows more than bank0" todo (`MILESTONES.md:262`).
- P02 notes/m14-smp.md read: 6 cores work (A55 x4 + A75 x2) via ACPI parking protocol, serialized CPU_ON (m14-smp.md:110-113,118-119,155-161); offering the M4 pair kills Windows early (m14-smp.md:129-136, 170-175), "Needs kernel-level debugging (KDNET/USB) - parked" (m14-smp.md:175). Defaults mask 0x3E, REAL_MPIDR 0.
- P03 notes/m13-ram.md read: Windows TotalVisibleMemorySize 5 499 168 KB = 5.24 GiB, TotalPhysicalMemory 5 631 148 032 B after boot #48 (m13-ram.md:34-37). Live /proc/iomem System RAM listed at m13-ram.md:8-9. Carve-outs listed m13-ram.md:25-29.
- P04 drivers/S10eDiag/esp/extra-kd.cmd read: KDNET over USB EEM, inbox kd_8003_5143.dll, DWC3 device role, DBG2 entry 0 -> \_SB.UFN0, host windbg -k net:port=50005,key=1.2.3.4 (extra-kd.cmd:3-6,20-23).
- P05 notes/m17-usb-kd.md read: KD over USB WORKS since boot #77 (m17-usb-kd.md:243-250): DBG2 0x8003/0x5143 type 5 busparams 0, MAX77705 COM switch to USB-AP, SUSPHY cleared; host WinDbgX -k net:port=50005,key=1.2.3.4 (m17-usb-kd.md:148-151); boot #78 = KD image without attach probe (m17-usb-kd.md:253-254). Target boots with debug on to desktop (m17-usb-kd.md:184-186). Phone is cabled to this PC (same cable as adb) (m17-usb-kd.md:159).
- P06 psci-parking.md C2 (notes/psci-parking.md:1013-1029): 9820 monitor DT /psci = bare "arm,psci" 0.1 binding with 0.2 IDs, cpu_on 0xC4000003. Topology (notes/cpu-driver-9820.md:108-115): cpu@0000-0005 + cpu@0100-0101; ananke(0-3) prometheus(4-5) cheetah(6-7); cpu-map cluster0 = 4+2, cluster1 = 2 M4. So "2+2+4" in the task = 4 A55 + 2 A75 + 2 M4 in 2 MPIDR clusters (6+2).
- P07 git log: no M4 experiment since commit 054c7be (run 34, boot #64/#65); KD went live later (boots #77/#78). So the M4 hang was never looked at under the debugger.
- P08 HANDOFF read: 6 CPUs + M4 hang summary (HANDOFF.md:1089-1094); default image = USB host, KD image = boot #78 separately (git 7879072; HANDOFF.md:1156); "KD auto-role (device when VBUS in, host otherwise)" still a todo (HANDOFF.md:1162-1163); current boot = #94+ with stay.flg (HANDOFF.md:1180-1183). Unattended loop mechanics HANDOFF.md:1077-1082.
- P09 source: BEYOND0LTE_SMP_PARKING 1, SAFE/PARK mask 0x3E (firmware/Platform/Beyond0LtePkg/AcpiTables/Beyond0LteAcpiHeader.h:56-67), FADT_PSCI = 0 when parking (:69-75); BEYOND0LTE_SMP_REAL_MPIDR 0 (Drivers/Beyond0LteAcpiPlatformDxe/Beyond0LteParking.c:62); USB_KD_DEVICE 0 = host image, 1 = KD image (Drivers/Beyond0LteUsbHostDxe/Beyond0LteUsbHostDxe.c:50); WINDOWS_WATCHDOG 0 (Drivers/Beyond0LteBootLogDxe/Beyond0LteBootLogDxe.c:49); PUBLISH_HIGH_BANKS 1 (Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c:38).
- P10 Madt.aslc: GICC CPU-interface number (GicId) = 0..7 = UID (AcpiTables/Madt.aslc:152-166); M4 GICC 6/7 MPIDR 0x100/0x101 eff 2 (:165-166). The M4s' real GIC CPU-interface number has never been read (no ITARGETSR read anywhere - checked below).
- P11 park stub (Drivers/Beyond0LteAcpiPlatformDxe/AArch64/Beyond0LteParkStub.S:15-21,46-52) logs only CurrentEL + MPIDR_EL1 + alive mark; no MIDR / ID_AA64* / CTR_EL0 / GICD_ITARGETSR0 read. rg over notes/docs: no per-core ID-register data exists for the M4 (only CTR_EL0 use in notes/m15-perf.md:53-57, unrelated).
- P12 RAM accounting (computed from Exynos9820.h:110-121 banks and the PlatformMemoryMapLib.c:110-129 reservation table, FB = 1080x2280x4 page-rounded, Exynos9820.h:882-887): live System RAM 6034 MiB; carved by us 659.9 MiB; published 5374.1 MiB; Windows sees 5370.3 MiB (m13-ram.md:34-35) -> 3.8 MiB to firmware allocations. vs 6144 MiB: 110 MiB holes never System RAM even for Linux (0xBAB00000-0xC0000000 85 MiB, 0xE0000000-0xE1900000 25 MiB; m13-ram.md:8-9) + 660 MiB our carve-outs.
  Carve-outs MiB: modem block 0xD0000000-0xD9801000 152; modem_if..4G 0xF0000000-0x100000000 256 (of which debug_snapshot 0xFD900000.. ~39); sec_rdx_bootdev 173; secure_camera 34; el2_earlymem 32; FB 9.4; el2_code 2; kaslr 1; seclog 0.5.
- P13 K10-rkp.md:322-327: sec_rdx_bootdev is "no-ship" in Samsung's exynos9820-rmem.dtsi; live reserved-memory table notes/K10-rkp.md:336-350 (el2_code live size 0x2000 and kaslr 0x80001000+0x1000 - our table carves 0x200000 / 0x80100000+0x100000, Exynos9820.h:144-147 vs :183-186 - tiny, but inconsistent). debug_snapshot log_pstore 0xEB308000 and log_s2d 0xEB310000+0x1500000 sit INSIDE published bank2 (K10-rkp.md:349-350) - hazard note, not capacity.
- P14 stale tracker row: MILESTONES.md:262 "Give Windows more than bank0 ... high banks off for UEFI (B63)" is still todo, but m13 boot #48 published the high banks and Windows saw 5.24 GiB (m13-ram.md:30-37; PlatformMemoryMapLib.c:38). The B63 limit no longer explains anything.
- P15 no SMBIOS anywhere in firmware/Platform/Beyond0LtePkg (rg -i smbios: no hits) -> Windows has no "installed RAM" figure, only usable.
- P16 KD host tooling: console kd.exe/cdb.exe exist at C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\ (ls); prior sessions used WinDbgX interactively (m17-usb-kd.md:148-151, 182-183, 202); no unattended KD runner script exists in firmware/tools (ls: pack-uefi*, repack-with-ref.py, s10e-cycle.sh, s10e-stage-kd.sh). s10e-stage-kd.sh pushes extra-kd.cmd + ARM64 kdnet set to the ESP from TWRP (firmware/tools/s10e-stage-kd.sh:1-4,12-16).
- P17 BIG cluster clock: ACPM FREQ_REQ only ever sent for LITTLE/MID/INT (notes/m15-perf.md:89-101); BIG never requested - the M4s run at whatever sboot left (unknown).
- P18 watchdog: in the m14 runs the 82 s cl0 watchdog was armed through EBS and fed only by the UFS driver while Windows wrote (docs/exynos9820-port/HANDOFF.md:1039-1040, 1056-1057). So the run-17 "reset without crash record" (m14-smp.md:131-134) fits a Windows HANG ended by our own watchdog, not an SoC fault. Today BEYOND0LTE_WINDOWS_WATCHDOG is 0 (Beyond0LteBootLogDxe.c:49), so a hang now stays hung (good for a KD break-in).
- P19 findings and ranked plan written (sections below). Task complete (read-only; no device, no build, no commit).

## Findings - CPU

1. **Windows already runs 6 of the 8 cores** (4x A55 + 2x A75), not 1. The user confirmed it on boot #58 (notes/m14-smp.md:160-161, commit df979ac; HANDOFF.md:1089-1092). The mechanism is the ACPI Multiprocessor Parking Protocol v1, not PSCI. UEFI CPU_ONs each secondary (SMC 0xC4000003, notes/psci-parking.md:1018-1024) one at a time into a polling stub, then patches GICC ParkedAddress in the MADT (m14-smp.md:53-58, 158-161). FADT ArmBootArch = 0 (no PSCI), because Samsung's EL3 is PSCI 0.1 and Windows' SYSTEM_RESET bugchecked 0xA0 (Beyond0LteAcpiHeader.h:48-53, 69-75).
2. **Topology**: 2 MPIDR clusters (6+2) and 3 efficiency classes (4+2+2). Nodes cpu@0000-0005 and cpu@0100-0101: ananke 0-3, prometheus 4-5, cheetah (M4) 6-7 (notes/cpu-driver-9820.md:108-115; Madt.aslc:25-32, 151-166). MADT GICC: GicId = UID = 0..7, MPIDR 0x0-0x5, 0x100, 0x101 (Madt.aslc:152-166). Efficiency classes are 0/1/2 since the inversion fix (Madt.aslc:63-73; m14-smp.md:177-184).
3. **Real MPIDR_EL1 differs from the MADT values.** A55/A75 report 0x81000n00 (MT=1, core number in Aff1); the M4s report 0x80000100/0x80000101 (MT=0) (m14-smp.md:98-100, 129-130, 162-165). For A55/A75 the mismatch is harmless (m14-smp.md:110-113).
4. **PSCI CPU_ON returns 0 for all 7 secondaries, including both M4s.** The M4s reach the stub (alive mark, EL1), so starting a 9820 big core does NOT hang the SoC, unlike the 9810 (m14-smp.md:129-131).
5. **What fails is Windows itself, once an M4 is offered.** Run 17 (mask 0xFE): S10eDiag loaded at 73 s, then nothing, no bugcheck record, then a reset (m14-smp.md:129-135). That pattern fits a hang ended by the watchdog (P18). Run 34 (REAL_MPIDR, only M4#1 offered, at a unique MPIDR): Windows died before S10eDiag loaded (m14-smp.md:170-175). That refutes the MPIDR-collision theory. Verdict then: "Needs kernel-level debugging (KDNET/USB) - parked" (m14-smp.md:175). Defaults restored: PARK/SAFE mask 0x3E, REAL_MPIDR 0 (Beyond0LteAcpiHeader.h:62-67; Beyond0LteParking.c:62).
6. **Never measured on the M4**: MIDR, ID_AA64ISAR0/1, PFR0, MMFR0-2, DFR0, CTR_EL0, its banked GICD_ITARGETSR0 (= its real GIC CPU-interface number), its clock. The stub records only CurrentEL + MPIDR_EL1 (Beyond0LteParkStub.S:15-21, 46-52). BIG-cluster DVFS was never requested (m15-perf.md:89-101). The MIDR of prometheus is itself disputed (psci-parking.md:1046-1054).
7. **KD exists, but has never been used on the M4 problem.** KDNET over USB EEM has worked since boot #77 (notes/m17-usb-kd.md:243-250). It connects at uptime 0:00:04 with 1 processor, BEFORE the parked cores start (m17-usb-kd.md:245-248), so it is attached when an M4 starts. There has been no SMP experiment since commit 054c7be (P07).

### Hypotheses for the M4 failure (ranked by estimate, all UNVERIFIED)
- H1 **ID-register / feature asymmetry** between Samsung's custom core and the ARM A55/A75. Windows refuses (e.g. bugcheck 0x3E MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED) or uses a feature that faults on one core type. Run 34's early death fits. If this is the cause and Windows insists on symmetry, there may be no firmware fix.
- H2 **BIG cluster clock/voltage** still in sboot's state (never FREQ_REQ'd, P17). A very slow M4 would miss Windows' processor-start / clock-IPI timing (a CLOCK_WATCHDOG-type hang).
- H3 **M4 GIC CPU-interface number is not GicId 6/7**, so IPIs are misrouted and the system freezes (fits run 17's silent hang). Lower prior: interfaces 0-5 clearly map 1:1 (6 cores work), which leaves only 6/7 on an 8-interface GICv2.
- H4 **MT=0 vs MT=1** topology confusion in Windows' processor numbering. Weak: run 34 used real MPIDRs and still died.

## Findings - RAM

1. **Windows sees 5 370 MiB** (TotalVisibleMemorySize 5 499 168 KB, TotalPhysicalMemory 5 631 148 032 B) since boot #48 (notes/m13-ram.md:34-37; m14-smp.md:111). The high banks ARE published (PlatformMemoryMapLib.c:34-38). The "high banks off for UEFI (B63)" / "multi-region RAM for winload" row (MILESTONES.md:262) is stale and explains none of the gap.
2. **DRAM is 3 x 2 GiB = 6 144 MiB, but live System RAM (DTB, /proc/iomem) is only 6 034 MiB**: 0x80000000-0xBAAFFFFF, 0xC0000000-0xDFFFFFFF, 0xE1900000-0xFFFFFFFF, 0x880000000-0x97FFFFFFF (m13-ram.md:8-9; Exynos9820.h:110-121). The 110 MiB of holes (0xBAB00000-0xC0000000 = 85 MiB, 0xE0000000-0xE1900000 = 25 MiB) are not RAM even for Android. Unrecoverable.
3. **We carve out 660 MiB more** (PlatformMemoryMapLib.c:110-129; constants Exynos9820.h:142-202). Published 5 374.1 MiB vs 5 370.3 MiB seen by Windows: the table explains the whole gap (P12 arithmetic).

| region | base | MiB | owner / evidence | recoverable? |
|---|---|---|---|---|
| modem_if + cp_ram_logging + debug_snapshot (up to 4 GiB) | 0xF0000000 | 256 | CP + Samsung logs (K10-rkp.md:336-350) | modem_if/cp_ram (~217): likely, the CP is never booted. Keep debug_snapshot (~39): reset logs, AGENTS §9 |
| modem_spare + modem_if2 + cp2cp + s5100_msi | 0xD0000000 | 152 | CP (Exynos9820.h:162-169; IF2/CP2CP are vendor-DT values, not live reads) | likely; needs a write probe |
| sec_rdx_bootdev | 0x880000000 | 173 | "no-ship" ramdump region (K10-rkp.md:322-327); EL1 reads at 0x880000000 / 0x88AD00000 OK (m13-ram.md:21-23) | likely; writes never tested |
| secure_camera | 0x96000000 | 34 | "likely TZ-protected" (PlatformMemoryMapLib.c:114) | maybe; risky |
| el2_earlymem + el2_code | 0x97E000000 / 0x87000000 | 34 | uH (PlatformMemoryMapLib.c:96-102) | no |
| boot framebuffer | 0xCA000000 | 9.4 | panel scan-out | no |
| kaslr, sec_debug_magic, seclog | - | 1.5 | sboot | no |

   **Ceiling** if every "likely" works: ~5 370 + 173 + 152 + 217 (+34) = ~5 910-5 946 MiB (~5.8 GiB). Windows can never show 6.0 GiB usable.
4. **No SMBIOS tables are produced** (P15), so Task Manager has no "installed" figure to show 6 GB against.

## Ranked plan - (likelihood of success) / (boot cycles)

| # | experiment | boots | KD? | why this rank |
|---|---|---|---|---|
| 1 | **Zero-boot TWRP reads** (R0+C0): `cat /proc/meminfo` (does Linux itself get the rdx/modem pages back?); `dmesg` filtered for reserved/rdx/SANITY/variation/"Booted secondary". Linux's own "SANITY CHECK: Unexpected variation in SYS_ID_*" lines would name exactly the asymmetric registers (H1), and the "Booted secondary processor" line gives the M4 MIDR | 0 | no | free; decides H1 and which carve-outs Android reclaims |
| 2 | **RAM write-probe from Windows** (R1): extend drivers/S10eDiag/esp/extra-ramprobe.cmd (run-11 pattern, m13-ram.md:14-24) to read/write/read-back/restore one word at 0x880000000, 0x885000000, 0xD0000000, 0xD1600000, 0xD5800000, 0xF0000000, 0xF8000000, 0xFB900000, and secure_camera 0x96000000 LAST; a mark before each. An abort becomes a bugcheck that S10eDiag records | 1 | no | high likelihood; rides on the normal 6-core image |
| 3 | **Publish what the probe cleared** (R2): drop the rdx / modem rows from mMonitorReservations (keep debug_snapshot 0xFD900000.. and uH); extra-mem.cmd confirms. Optional: SMBIOS Type 16/17/19 (6 GiB installed) | 1 | no | +~540 MiB expected; one commit per carve-out group |
| 4 | **M4 probe without offering it to Windows** (C1): a new PROBE mask (CPU_ON + log, MADT-disabled) for GICC 6/7. The stub also stores MIDR, ID_AA64ISAR0/1, PFR0, MMFR0-2, DFR0, CTR_EL0, GICD_ITARGETSR0 (32-bit at GICD+0x800) and a CNTPCT-timed loop (clock estimate) in the mailbox's firmware half; UEFILOG compares A55 / A75 / M4 | 1 | no | settles H1/H2/H3 while Windows keeps 6 cores; CPU_ON of an M4 is proven safe (m14-smp.md:129-131) |
| 5 | **KD run with the M4s offered** (C2): KD image (BEYOND0LTE_USB_KD_DEVICE 1, Beyond0LteUsbHostDxe.c:50 - USB host/dock off) with PARK mask 0xFE. BCD debug must already be on before this boot: set it offline (elevated `bcdedit /store <pulled ESP BCD> /debug on`, precedent m17-usb-kd.md:162-163), or run one boot of a 0x3E KD image with s10e-stage-kd.sh + extra-kd.cmd. Host: `kd.exe -k net:port=50005,key=1.2.3.4 -logo <log>` (x64 Kits, P16), started before `adb reboot`, symbols srv*. On break: `!analyze -v`, `!running -ti`, `~*k`, `!prcb` per CPU | 1-2 | **yes** | highest certainty about the failure itself, but costlier: the KD image drops USB host, and there is no unattended KD runner in the repo, so a break-in for a hang needs a human Ctrl+Break or a small helper |
| 6 | **Fix by result** (C3): H3 -> patch GICC CPUInterfaceNumber from the probed ITARGETSR. H2 -> ACPM FREQ_REQ for BIG behind a progress guard, like LITTLE/MID (m15-perf.md:89-101). H1 -> evaluate a Windows-side patch (DuoWoA KernelErrataPatcher precedent, psci-parking.md A5.3) or accept 6 cores | 1+ each | after #4/#5 | contingent |

**Order for today**: 1, then 2 and 4 in the SAME image (neither needs KD; one boot), then 3. Run 5 only if 4 + the dmesg read do not already name the cause.
- 2026-10-12 ~11:00 (main session) plan #1 done (TWRP reads, boot #99 era):
  - TWRP Linux MemTotal 5 548 572 kB (5 418 MiB).
  - dmesg CPU6/7 (M4, MIDR 0x531f0030): "SANITY CHECK: Unexpected variation in SYS_ID_AA64ISAR1_EL1. Boot CPU:
    0x100001, CPU6: 0x1" and "SYS_ID_AA64MMFR1_EL1 Boot CPU: 0x10212122, CPU6: 0x10211102" -> "Unsupported CPU
    feature variation detected". A55 MIDR 0x411fd050, A75 0x412fd0a1.
  - Decode: ISAR1[23:20] LRCPC = 1 on A55/A75, **0 on the M4** (no LDAPR). MMFR1 [7:4] VMIDBits 2 (16-bit) vs 0
    (8-bit), [15:12] HPDS 2 vs 1. => **H1 (feature asymmetry) is real**: the M4 lacks RCPC and 16-bit VMID.
    Windows sizes features from the boot CPU (A55); code using LDAPR (RCPC) on an M4 = undefined instruction.
- plan #2: drivers/S10eDiag/esp/extra-ramprobe2.cmd (rdx, modem_spare/if2/cp2cp, modem_if, cp_ram_logging,
  secure_camera last; log %OUT%\last\ramprobe2.txt).
- **Run r2 RESULT (boot #99 + extra-ramprobe2.cmd):** every probed carved region was readable and writable from
  Windows (EL1) with read-back 0x5A5AA5A5 and no fault: sec_rdx_bootdev (0x880000000 / 0x885000000 /
  0x88ACFF000), modem_spare 0xD0000000, modem_if2 0xD1600000 / 0xD3000000, cp2cp 0xD5800000 / 0xD97FF000,
  modem_if 0xF0000000 / 0xF6000000 / 0xFB8FF000, cp_ram_logging 0xFB900000 / 0xFD8FF000, secure_camera
  0x96000000 (read 0, write OK). Windows ran normally afterwards and returned to TWRP.
- Plan #3 (publish them as RAM, keep debug_snapshot 0xFD900000..4 GiB; ~+576 MiB) was **not applied**: the
  memory-map edit was refused by the session's safety classifier (it removes secure/firmware reservations).
  Waits for the user's decision.
