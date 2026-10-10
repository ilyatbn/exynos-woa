# S10e (Exynos 9820) reboot-to-recovery, SWRESET, watchdog, PS_HOLD - read from Samsung's own binaries

Source: subagent disassembly 2026-10-10 of the stock kernel (s10e-artifacts\boot.img, vmlinux-to-elf symbols,
base 0xffffff8008080000) and sboot.bin (BL_G970FXXSGHWC1 tar in C:\Users\ilya\code\Dumps). Scratch scripts and
listings: %TEMP%\claude\...\scratchpad\kernel\ (extract.py, run_v2e.py, rela.py, ofmatch.py, scanmov.py).

PMU base 0x15860000 (DT /system-controller@15860000, /reboot pmu_base; sboot uses 0x1586xxxx absolute).
exynos_pmu_update (kernel 0x861c5e8) for off < 0x4000: set bit n -> write n to PMU+(off|0xC000); clear -> PMU+(off|0x8000).

## `reboot recovery` (sec_reboot 0x8e14574 -> exynos_reboot 0x8638fd8)
1. INFORM2 PMU+0x808: RMW bits[23:16] = 0x4E (0x8e145bc..0x8e14610)
2. INFORM3 PMU+0x80C = 0x12345674 (strncmp "recovery" 0x8e14650 -> 0x8e14880 mov/movk 0x12345670 +4 -> 0x8e1497c)
3. flush_cache_all, llc_flush, mach_restart(3,"sw reset")
4. exynos_acpm_reboot() (IPC to APM)
5. (0x810 written only for charge/bootloader/recovery cmd strings; cmd here is "sw reset" -> nothing)
6. PMU+0x404 (RSTSTAT) = 0 (0x86391e4)
7. SWRESET: PMU+0x3A00 = 0x2 (0x8639204..0x863920c), then wfi. sboot itself: sboot 0xa5200 str #2 -> 0x15863A00.
   (only when chipid product_id != 0xE9810000 - the 9810's id)
INFORM3 decode in sboot s5p_check_reboot_mode (0xac430): (v & ~0xF) == 0x12345670 -> REBOOT_MODE = v & 0xF;
recovery path sets REBOOT_MODE 4. Other modes: normal +0, download +1, upload +2, recovery-update +5, fota_bl +6.
sboot register names: RSTSTAT 0x404, INFORM0..3 0x800/0x804/0x808/0x80C, INFORM4 0x840, INFORM5 0x844, INFORM7 0x84C.
The 9810 values (SWRESET 0x400=1, PS_HOLD 0x330C) are WRONG here.

## Watchdog cl0 0x10050000 (samsung,exynos9-v1-wdt; DT timeout-sec 30; clock fin_pll 26 MHz)
WTCON +0, WTDAT +4, WTCNT +8, WTCLRINT +0xC. v1 variant: NO disable/mask-reset regs; NONCPU_INT_EN PMU+0x1244 bit 2
set by the kernel before use (exynos_pmu_update(0x1244, 1<<2, 1<<2)); rst_stat 0x404 bit 24.
Start: freq = ceil(clk/128); count = T*freq; if >= 0x10000 divisor = ceil(count/0xFFFF) (max 0x100), WTCON[15:8] =
divisor-1; WTDAT = WTCNT = ceil(count/divisor); WTCON = (WTCON & ~0x3D) | 0x39 (RSTEN bit0, DIV128 [4:3]=11, EN bit5).
At 26 MHz: 60 s -> WTCON 0xB939, WTDAT/WTCNT 0xFFF5; 30 s -> 0x5C39/0xFFF5; max ~82.6 s. Stop: WTCON &= ~0x21.
Immediate reset (s3c2410wdt_restart): WTCON=0; WTCNT=0x80; WTDAT=0x80; WTCON=0x2021.
UNVERIFIED: whether NONCPU_INT_EN bit2 is required for the reset; the 26 MHz rate is a DT/binary reading, not measured.

## PS_HOLD / power-off
PS_HOLD_CONTROL PMU+0x30C bit 8 cleared = power off (sec_power_off 0x8e144b0, "set PS_HOLD low").
