# M12 - ACPM IPC client in UEFI: enable the touch rails (LDO19M tsp_io, LDO27M tsp_avdd)

Goal: Beyond0LteTouchPowerDxe - at ReadyToBoot, talk to the APM (ACPM firmware, owns the S2MPS19 PMIC)
over the acpm_ipc mailbox, read L19CTRL/L27CTRL, set enable field [7:6] to 0b11 if not, read back, settle.
Context: touch run 9 (notes/m12-touch.md) - I2C lines low, rails suspected off under Windows.

Kernel source of truth: LineageOS android_kernel_samsung_exynos9820, branch lineage-22.2 (raw.githubusercontent).
Local copies: %TEMP%\acpm-src\ (acpm_ipc.c/.h, acpm_mfd.c, include/soc/samsung/acpm_mfd.h, acpm_ipc_ctrl.h,
fw_header/framework.h + common.h, sec_ts y771/y771_d, include/linux/mfd/samsung/core.h + s2mps19.h),
%TEMP%\exy-s2mps19_core.c / exy-s2mps19_regulator.c / exy-s2mps19-regulator.h.

## Progress log

- 2026-10-11 P1 (protocol, kernel read):
  - acpm_ipc.h:80-89 mailbox offsets: INTGR0 0x08, INTCR0 0x0C, INTMR0 0x10, INTSR0 0x14, INTMSR0 0x18,
    INTGR1 0x1C, INTCR1 0x20, INTMR1 0x24, INTSR1 0x28, INTMSR1 0x2C. IPC_TIMEOUT 15000000 ns (acpm_ipc.h:95).
  - acpm_ipc.c:801-808 initdata = sram_base + initdata-base (DT 0x7000) -> struct acpm_framework (framework.h:37-81).
    Under CONFIG_EXYNOS_ACPM all pointers are u32: plugins@0x00 num_plugins@0x04 ipc_channels@0x08
    num_ipc_channels@0x0C pid_framework@0x10 pid_max@0x14 ipc_ap_max@0x18 ... (layout computed from the header).
  - acpm_ipc.c:709-755 channel_init: num_channels = initdata->ipc_ap_max; ipc_ch table = sram + initdata->ipc_channels,
    struct ipc_channel (framework.h:123-130) = id, field, owner, type, channel_info(13 x u32, framework.h:97-111),
    ap_poll -> 18 x u32 = 72 bytes. AP TX queue = APM's rx_* (rx_rear/rx_front/rx_base), AP RX queue = APM's tx_*
    (tx_rear/tx_front/tx_base); elem size q_elem_size, length q_len; offsets are relative to sram_base.
  - acpm_ipc.c:223-261 request_channel: DT "acpm-ipc-channel" of the client node, matched against channel[i].id.
  - acpm_mfd.c:19-61 exynos_acpm_read_reg: cmd[0] = (type&0xF)<<8 | reg | (channel&0xF)<<12; cmd[1] = FUNC_READ(0);
    result: dest = (cmd[1]>>8)&0xFF, ret = (cmd[1]>>24)&0xFF (nonzero = APM speedy fail). write_reg (acpm_mfd.c:110-149):
    cmd[1] = FUNC_WRITE(1) | value<<8. update_reg: FUNC_UPDATE(2) | val<<8 | mask<<16. Shifts/masks acpm_mfd.h (include/soc).
  - s2mps19_core.c:52-59: channel (speedy channel) = 0, type = i2c->addr = I2C_ADDR_PMIC 0x01 (s2mps19_core.c:41).
  - acpm_ipc.c:504-628 __acpm_ipc_send_data: front/rear of TX; next=front+1 wrap at len; wait until next != rear
    (queue not full, UNTIL_EQUAL 15 ms); seq_num 1..63, cmd[0] |= seq<<16 (ACPM_IPC_PROTOCOL_SEQ_NUM 16,
    acpm_ipc_ctrl.h); copy q_elem_size bytes to tx_base + size*front; write front=next; apm_interrupt_gen:
    writel((1<<id)<<16, intr+INTGR0) (acpm_ipc.c:427-431). If polling+response: loop while
    !(INTSR1 & (1<<id)) || check_response(); udelay(10) per iteration, 15 ms timeout (+retries).
  - acpm_ipc.c:282-345 check_response: scan rx from rear to front for element whose (word0>>16)&0x3f == seq;
    copy out; if i != rear move rear's element into slot i; rear=(rear+1)%len, write; if rear==front after re-read:
    write (1<<id) to INTCR1, and if front moved meanwhile re-raise via INTGR1.
  - memcpy_align_4 (acpm.c:236) = 32-bit word copy.
- 2026-10-11 P2 (DT, stock dtbo.dtb = s10e-artifacts\dtbo.dtb, first FDT of the G970F dtbo.img):
  /fragment@70/__overlay__/s2mps19mfd@00: acpm-ipc-channel = 2, acpm-speedy-channel = 0.
  Live DT archive (dt-acpm-20261011.tar) does NOT contain the s2mps19mfd node -> channel 2 is from the stock DTBO,
  not the live tree. Read that would verify on device (TWRP): xxd
  /proc/device-tree/speedy@15970000/s2mps19mfd@00/acpm-ipc-channel (expect 00000002).
  Other acpm-ipc-channel users (stock dt.dtb): cmu 0, exynos-dm/devfreq 1, acpm 4, dvfs 5, tmu 9, sci 10.
- 2026-10-11 P3 (regulator semantics):
  - s2mps19_regulator.c:93-104 of_map_mode: SEC_OPMODE_SUSPEND(1)->0b01, SEC_OPMODE_MIF(2)->0b10, ON(3)->0b11
    (enum in include/linux/mfd/samsung/s2mps19.h:58-65). s2m_enable (s2mps19_regulator.c:120-127) =
    update_reg(enable_reg, opmode<<6, 0xC0).
  - stock dtbo fragment@31 LDO19M (tsp_io 1.8 V): regulator-boot-on, regulator-initial-mode 2 -> Android enables
    LDO19M with field 0b10 (ON while PWREN_MIF high), NOT 0b11. LDO27M (tsp_avdd 3.0 V): initial-mode 3 -> 0b11.
    So 0b10 on L19CTRL is a legitimate "on" value under Android. Driver per task writes 0b11 when field != 0b11
    (0b11 = on in normal mode, superset of 0b10 while awake); logged explicitly.
  - S2MPS19_ENABLE_TIME_LDO 128 us (s2mps19-regulator.h:225).
  - sec_ts y771_d sec_ts.c:2043-2090 sec_ts_power(on): enable dvdd (tsp_io), sec_ts_delay(1), enable avdd;
    probe sec_ts.c:2722-2724: sec_ts_power(true); if (!pdata->regulator_boot_on) sec_ts_delay(70).
    -> order LDO19M first, 1 ms, LDO27M, then 70 ms settle.
- 2026-10-11 P4 (repo state + design decisions):
  - PlatformMemoryMapLib mMmioRegions had NEITHER 0x15900000 (mailbox) NOR 0x02039000 (APM SRAM) -> adding both
    (DEVICE) via new Exynos9820.h entries EXYNOS_ACPM_IPC_MBOX_* / EXYNOS_ACPM_SRAM_* (live DT acpm_ipc reg).
  - BootLogDxe flushes at ReadyToBoot (TPL_CALLBACK). The touch-power handler is created at TPL_NOTIFY so it runs
    BEFORE that flush (higher TPL dispatched first) and its lines land in \UEFILOG.TXT of the same boot. Once per boot.
  - Enable uses FUNC_UPDATE (val 0xC0, mask 0xC0) = the kernel's own s2m_enable path (s2mps19_regulator.c:120-127,
    update_reg), so the APM keeps bits [5:0] (voltage) by construction. Read back with FUNC_READ.
  - Deviation from the kernel (documented in code): seq number picked to not collide with any element already pending
    in the AP RX queue (sboot may leave unconsumed replies; kernel starts at seq 1 after a clean probe).
  - Timeouts: queue-full wait 15 ms (UNTIL_EQUAL, IPC_TIMEOUT), reply wait 100 ms (kernel: 15 ms + up to 6 retries
    and then a WDT emergency reset - we log and stop instead). After any IPC failure no further PMIC command is sent.
  - FILE_GUID 11111111-2222-3333-4444-555555555551 (551 unused in firmware/**.inf).
- 2026-10-11 P5 (code written, uncommitted): Exynos9820.h ACPM section (mailbox 0x15900000+0x1000, SRAM
  0x02039000+0x30000, initdata 0x7000, all VERIFIED live DT); PlatformMemoryMapLib maps ACPM_MBOX + ACPM_SRAM as
  DEVICE; new Drivers/Beyond0LteTouchPowerDxe (.c/.inf, GUID ...551), wired into Beyond0LtePkg.dsc/.fdf after
  BootLogDxe. Compile-time BEYOND0LTE_TOUCH_POWER_WRITE (default 1; 0 = read-only). Next: build.
- 2026-10-11 P6 (build): build-uefi.ps1 -Platform Beyond0LtePkg exit 0, -Werror, 0 warnings; Guid.xref lists
  ...551 Beyond0LteTouchPowerDxe; .efi 15872 B. FVMAIN_COMPACT 99% full (3384 B free) - next DXE added may not fit.
- 2026-10-11 Review (adversarial, mechanical):
  - struct offsets recomputed from framework.h field lists: acpm_framework ipc_channels 0x08, ipc_ap_max 0x18,
    build_info 0x60; ipc_channel stride 0x48 (4 + 13 + 1 u32, no padding), ch fields 0x10..0x40, ap_poll 0x44. OK.
  - Citations re-grepped: acpm_ipc.c 430/531/545-548/564/573, acpm_mfd.c 28-30/43-44/121/211-213/226,
    mfd.h FUNC_UPDATE line 53, framework.h TYPE_QUEUE 240, s2mps19-regulator.h 114/122/220/221/225, sec_ts.c
    2072/2083/2086, s2mps19_regulator.c 99-100. One wrong: s2mps19_core.c "channel = 0" is line 55 not 54 - fixed.
  - Mailbox/SRAM windows do not overlap existing map entries (PMU 0x15860000-0x15870000, SPEEDY 0x15970000,
    ALIVE 0x15850000-0x15851000; nothing below 0x80000000 except CHIPID 0x10000000).
  - Open: PMIC channel 2 is from the stock DTBO, not the live tree (UNVERIFIED, named so in code).
  - Open: LDO19M's Android opmode is 0b10 (initial-mode 2 = SEC_OPMODE_MIF); the driver writes 0b11 when it reads
    0b10. Harmless while awake (0b11 = on in normal mode) but it is not Android's value; if 0b10 is read, the rail
    was very likely already on and the I2C-low problem is elsewhere.
- 2026-10-11 (main session): live DT read in TWRP: speedy@15970000/s2mps19mfd@00/acpm-ipc-channel = 00000002
  => ACPM_PMIC_IPC_CHANNEL VERIFIED (renamed from _UNVERIFIED). Boot #47 = first on-device run.
