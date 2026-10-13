# WWAN: mobile data (Exynos 9820 integrated CP, project S5000AP) — research + Windows driver

Task: "9820 4g model. i wanna try getting mobile data working." Mode: **research** (evidence →
constants → design → next order of work; build only if the contract turns out clear enough).
Started 2026-10-12. Follows D4b WiFi (`notes/m12-wifi-chip.md` — same PCIe generation of questions)
and D4c BT (`notes/bt-driver.md`).

## Progress log

### 2026-10-12 — T0: framing — what the plan of record already closed about the modem (fact, cited to repo notes; the "S5100" reading here is superseded by T7 — the S10e uses the integrated CP)

- **The modem is a Shannon **S5100** ("s5100") subsystem**: the shipping rmem include is
  `exynos9820-rmem_s5100_dual.dtsi` and the base DT carries AP↔CP handshake pinctrl states named
  `s5100_evt1_dual_ap2cp_wakeup`, `..._ap2cp_cp_pwr_on`, `..._cp2ap_phone_active`,
  `..._cp2ap_wake_up`, `..._cp2ap_cp_ps_hold`, `..._ap2cp_nreset_n`, `..._ap2cp_pda_active`
  (`dumps/hhxj6/base.dts:803-850, 2393-2405`, states under `pinctrl@15C30000` and `pinctrl@10830000`).
- **The modem's memory carve-outs are VERIFIED** (`Exynos9820.h:155-164`, from the vendor DT):
  `modem_spare 0xD0000000+0x1600000`, `modem_if2 0xD1600000+0x4200000`,
  `cp2cp_shm 0xD5800000+0x4000000`, `s5100_msi 0xD9800000+0x1000`,
  `modem_if 0xF0000000+0xB900000`, `cp_ram_logging 0xFB900000+0x2000000`.
- **`pcie_1` (0x13120000, Gen3 x2) is the S5100 modem doorbell but is *disabled* on this board** —
  `notes/m12-wifi-chip.md:88`. So the AP↔modem path on the S10e is **not** PCIe; it is shared
  memory + GPIO handshake. (Our PCIe DXE work deliberately leaves pcie_1 untouched — HANDOFF §24.)
- **The shipped kernel has a built-in `mif` (modem interface) driver**: `strings kernel` shows
  `mif: SBD_IPC {shmem_base:... shmem_size:...}`, `SBD_IPC {version, num_channels, rbps_offset}`,
  `RB_DESC[..][DL/UL]` ring descriptors with `id/ch/sbdv_offset/rb_len`, `DIT RX`,
  `dit_tx_done_handler`/`dit_rx_done_handler` IRQ handlers, `AP2CP_CFG`, `CP_START`
  (`dumps/hhxj6/kernel`, strings read 2026-10-12). That is the AP↔CP IPC vocabulary this arc must
  pin down.
- **`cpefs` (the CP filesystem) is on `sdd1`, a separate UFS LUN** — `notes/dock-desktop.md:545-549`.
- **base.dts has no top-level modem device node** (node-name grep 2026-10-12): the modem device must
  be added by a dtbo overlay (`dumps/hhxj6/dts/dtb00..06.dtb`) or named something not yet grepped —
  open item U1.

### 2026-10-12 — T1: U1 CLOSED — the modem device nodes live in the dtbo overlays (VERIFIED, raw DTB walk)

Method: `python3 /tmp/opencode/modem-wwan/walk_dt.py dumps/hhxj6/dts/dtb0*.dtb dumps/hhxj6/base.dtb`
and `dump_frag.py` (both use `tools/dump-tools/fdt.py`, raw DTB, no dtc). Full subtree dump:
`/tmp/opencode/modem-wwan/frag_dtb02.txt`.

- **All 7 overlays are beyond0lte**: root `compatible` = `samsung,BEYOND0LTE EUR OPEN 17..25`
  (dtb00→17, 01→18, 02→19, 03→20, 04→22, 05→24, 06→25). The modem fragments are **byte-identical
  across all 7 SKUs** except phandle values and fragment index (VERIFIED: `diff` of the
  `dump_frag.py` output for all 7). So one contract covers every beyond0lte variant.
- **base.dtb (raw) has no modem device node** — confirms the earlier base.dts grep at node level.
  Only hits: the 9 `s5100_evt1_dual_*` pinctrl states + unrelated `camif-wdma`/`dsim`/`devfreq_mif`
  name noise. Also: base.dtb `/reserved-memory` has **no** modem children (all 10 children listed;
  only ion/mm_debug/sec_rdx/seclog etc.) — the modem rmem regions are themselves **added by the
  overlay** (fact).
- **Overlay fragment A** (`fragment@33` in dtb00/01, `fragment@34` in dtb02..06; `target =
  <0xffffffff>` = merge-time placeholder) adds, with `#address-cells=2 #size-cells=1`:
  - `modem_if` — `compatible = "exynos,modem_if"`, `reg = <0x0 0xf0000000 0xb900000>`
  - `cp_ram_logging` — `compatible = "exynos,cp_ram_logging"`, `reg = <0x0 0xfb900000 0x2000000>`
  - `modem_spare` — `compatible = "exynos,modem_spare"`, `reg = <0x0 0xd0000000 0x1600000>`
  These match the VERIFIED rmem constants in `Exynos9820.h:155-164` exactly.
- **Overlay fragment B** (`fragment@modemif`, `target-path = "/"`) adds the real device tree:
  `mif_pdata` + children `iodevs` (22 channels), `mailbox_cp`, `shmem`.

**`mif_pdata` — compatible `"sec_modem,modem_pdata"`, `status = "okay"`** (dtb02; all cells raw from DTB):
- `reg-names = "offset", "dit", "sysreg_busc"`;
  `reg = <0x0 0x10000 0x0>, <0x0 0x1a2a0000 0x10000>, <0x0 0x1a220000 0x2000>`
  → **DIT doorbell MMIO = 0x1A2A0000 (64 KiB)**, **sysreg_busc = 0x1A220000 (8 KiB)**.
  First entry ("offset", addr cell 0x10000, size 0) is not an MMIO window — interpretation
  UNVERIFIED (inference: an SBD/shmem offset parameter).
- `ap2cp_cfg_addr = <0x15020000>` (AP2CP sysreg, matches the `AP2CP_CFG` string T0 found in kernel).
- `interrupts = <0x0 0x177 0x0>, <0x0 0x44 0x0>, <0x0 0x43 0x0>, <0x0 0x42 0x0>`;
  `interrupt-names = "CP2AP_WAKEUP", "DIT_BUS_ERR", "DIT_RX_DONE", "DIT_TX_DONE"`.
  First cell 0 = SPI type (base GIC `/interrupt-controller@10100000`, phandle 1, `#interrupt-cells=3`
  in base.dtb). DT-relative SPIs **375, 68, 67, 66**; GIC INTIDs 407/100/99/98 (inference from the
  standard GIC +32 offset).
- `gpios` = 8 specifiers `<0xffffffff pin 0x0>`, pins **4,3,2,1 then 5,4,3,2** — phandles are
  merge-time placeholders, NOT resolvable from the raw .dtb (dead end: no fixup table in the blob;
  resolving needs the merged live DT, R01). The base-DT pinctrl states name the handshake pins:
  `ap2cp_wakeup gpm17-0`, `ap2cp_cp_pwr_on gpm20-0`, `ap2cp_dump_noti gpm25-0`,
  `2cp_uart_sel gpm26-0`, `cp2ap_phone_active gpm28-0` (pin-function 0xf), `cp2ap_wake_up gpm30-0`
  (0xf), `cp2ap_cp_ps_hold gpm31-0` (0x0), `ap2cp_nreset_n gph1-4`, `ap2cp_pda_active gph1-3`
  (base.dtb `/pinctrl@15C30000/...` and `/pinctrl@10830000/...`, dumped 2026-10-12).
- Identity/config scalars: `mif,name = "s5000ap"`; `mif,modem_type = <0xb>`;
  `mif,ipc_version = <0x32>` (50); `mif,link_types = <0x200>`; `mif,link_name = "shmem"`;
  `mif,link_attrs = <0x7c9>`; `mif,use_handover = <0x0>`; `mif,modem_net = <0x0>`;
  `mif,num_iodevs = <0x16>` (22).
- AP→CP / CP→AP interrupt-id maps (`mif,int_ap2cp_msg 0, _wakeup 1, _status 2, _active 3`;
  `mif,irq_cp2ap_msg 0, _status 2, _active 3, _perf_req_cpu 5, _perf_req_mif 6, _perf_req_int 7,
  _wakelock 8, _ratmode 9`) and mailbox ids (`mbx_ap2cp_msg 0, mbx_cp2ap_msg 1,
  mbx_ap2cp_united_status 2, mbx_cp2ap_united_status 3, mbx_cp2ap_dvfsreq_cpu/mif/int 5/6/7,
  mbx_ap2cp_kerneltime 9`).
- **SBI ("united status" shared word) bitfields** — mask/pos pairs, e.g.
  `sbi_cp_status_mask 0xf pos 0x1`, `sbi_ap_status 0xf@0x1`, `sbi_pda_active 0x1@0x5`,
  `sbi_cp2ap_wakelock 0x1@0x6`, `sbi_cp_evs_mode 0x1@0x7`, `sbi_ds_det 0x3@0xe`,
  `sbi_ext_backtrace 0x1@0x11`, `sbi_device_type 0x1f@0x12`, `sbi_ap2cp_kerneltime_sec 0xfff@0x14`,
  `sbi_ap2cp_kerneltime_usec 0xfffff@0x0`, `sbi_crash_type 0xf@0x17`, `sbi_sys_rev 0xff@0x6`,
  `sbi_cp_rat_mode 0x3f@0x1a`, `sbi_lte_active 0x1@0x5` (full list in frag dump).
- **Channel table (`iodevs`, 22 entries)** — name / `iod,id` / format / io_type / app / buffers:
  `umts_cass` 0x23 f1 t0; `umts_ipc0` 0xeb f0 t0 RIL (10×0x1000 UL, 32×0x1000 DL);
  `umts_ipc1` 0xec f0 t0 RIL; `umts_rfs0` 0xf5 f2 t0 RFS (DL 512×0x800); `umts_csd` 0x1 f1 t0 CSVT;
  `umts_router` 0x19 f1 t0 "Data Router"; `umts_dm0` 0x1c f1 t0 DIAG (DL 128×0x800);
  `ipc_loopback0` 0xf4 f0 t0 CLD; **`rmnet0..7` ids 0xa..0x11, f1, io_type 1, app "NET"** (the
  IP data path — 8 rmnet links); `multipdp_hiprio`/`multipdp` id 0 f3 t2 RIL;
  `umts_boot0` 0xd7 **f4** t0 CBD (boot channel); `umts_ramdump0` 0xe1 **f5** t0 CBD;
  `smd4` 0x21 f1 t0 (`iod,option_region "kor_skt"`); `umts_ciq0` 0x1a f1 t0 (`"usa_spr"`).
- **`mailbox_cp` — compatible `"samsung,exynos-shd-ipc-mailbox"`**, `reg = <0x0 0x15920000 0x180>`,
  `interrupts = <0x0 0x29 0x0>` (SPI 41), `mcu,name = "mcu_ipc_cp"`, `mcu,id = <0x0>`,
  `mcu,irq_affinity_mask = <0x3>`.
- **`shmem` — compatible `"samsung,exynos-shm_ipc"`** (no reg; partition of the `modem_if` window):
  `cp 0..0x7800000` (CP image), `vss @0x7800000 +0x800000`, `l2b @0x8000000 +0x3200000`,
  `ipc @0xb200000 +0x500000`, `vparam @0xb700000 +0x200000`, `zmb 0`. Sum = **0xB900000 = exactly
  the `modem_if` reg size** (arithmetic VERIFIED) → inference: shmem partitions the 0xF0000000
  window, so **SBD IPC structures live at physical 0xF0000000+0xB200000 = 0xFB200000** (UNVERIFIED
  until the kernel driver's use of `shmem,ipc_offset` is read).
- `ov00.dts` is the decompile of dtb00 (root compatible "…EUR OPEN 17"); its phandle annotations are
  garbled (`mif,modem_type = <&freq_1176>` for plain `<0xb>`) — confirms "prefer raw DTB".

### 2026-10-12 — T2: kernel inventory — the mif/SBD stack is `drivers/misc/modem_v1` (VERIFIED via strings; kallsyms is a dead end)

- **Dead end (documented, do not retry): `kallsyms.log`/`symbols.tsv` carry NO symbol names.**
  The shipped Image has no kallsyms; the 894-entry run is a .rodata coincidence
  (`tools/dump-tools/kallsyms.py` header states this NEGATIVE RESULT; re-confirmed 2026-10-12:
  `grep -icE "mif|modem|..." symbols.tsv` → 0). Inventory below is from
  `strings -a dumps/hhxj6/kernel > /tmp/opencode/modem-wwan/kernel.strings.txt` (291 408 lines).
- **Driver source paths embedded in the Image** (fact): `drivers/misc/modem_v1/modem_main.c`,
  `drivers/misc/modem_v1/modem_io_device.c`, `drivers/misc/modem_v1/include/sbd.h`,
  `drivers/misc/mcu_ipc/shm_ipc.c`. So the stack = **modem_v1 (mif)** + **mcu_ipc (shm_ipc/mailbox)**.
- **Match tables present**: `sec_modem,modem_pdata`, `samsung,exynos-shm_ipc`,
  `samsung,exynos-shd-ipc-mailbox` (kernel.strings.txt:48663-48668 — same compatibles as the T1
  overlay nodes → the overlay nodes ARE bound by this kernel), and `exynos,modem_if` /
  `exynos,cp_ram_logging` / `exynos,modem_spare` (217627-217629, claimed by shm_ipc.c's
  `modem_if_reserved_mem_setup` / `_cplog_setup` / `_spare_setup` + `deliver_cp_reserved_mem_setup`).
- **IPC flavor = SIPC5**: `mif_sipc5`, `sipc5_init_io_device`, `sipc5_build_config`,
  `sipc5_build_header`, and the guard `"IPC version %d < SIPC_VER_50"` — matches
  `mif,ipc_version = <0x32>` (50) from T1.
- **Module names / params** (fact): `modem_utils.dflags`, `modem_utils.wakeup_dflags`,
  `link_device_shmem.gro_flush_time`, `modem_ctrl_s5000ap.ds_detect`,
  `modem_argos_notifier.{lit,big}_rmnet[_clat]_rps`, `..._clat_rps`, `mif_rps_thresh`.
  → the pieces are `modem_ctrl_s5000ap` (controller ops), `link_device_shmem`, `modem_utils`,
  `modem_argos_notifier` (QoS/RPS).
- **GPIO property names the driver parses** (strings order, `parse_dt_common_pdata` +
  `__parse_dt_optional_gpio_pdata`): `mif,gpio_cp_wakeup` (label CP_WAKEUP), `mif,gpio_ap_status`
  (AP_STATUS), `mif,gpio_ap_wakeup` (AP_WAKEUP), `mif,gpio_cp_status` (CP_STATUS) — 4 named,
  requested via devm gpio; plus `get_system_rev` reads `gpio_hw_rev%d` with a `gpio_count` from DT
  ("failed to get gpio_count from DT") → the 8 DT gpio cells = 4 handshake + 4 hw-rev bits
  (inference from the two parse paths; UNVERIFIED which is which).
- **DIT driver** (`dit_init`): ioremaps reg-name `dit` ("DIT base address : 0x%llX") and
  `sysreg_busc` ("DIT busc ioremap"); requests IRQs by name `DIT_BUS_ERR`/`DIT_RX_DONE`/`DIT_TX_DONE`
  → `dit_bus_err_handler`/`dit_rx_done_handler`/`dit_tx_done_handler`; reads `DIT_STATUS`,
  `DIT_BUS_ERR` regs. DIT is a **HW packet classifier with NAT/CLAT offload**:
  `dit_set_nat_local_addr`, `dit_set_nat_filter` (id/proto/sa/sp/dp), `dit_set_clat_plat_prfix`,
  `dit_set_clat_saddr`, `dit_set_clat_filter`; `pktproc_gdesc_info` prints a **Global descriptor:
  {desc_base_addr, num_of_desc, fore_ptr, rear_ptr}** — the DIT ring. Netdev names `rmnet0/1`,
  `v4-rmnet`, `clat rmnet0`.
- **SBD shmem layout functions** (`setup_desc_rgn`/`setup_sbd_rb`): `SHMEM {base,size}` →
  `G_DESC_OFFSET`, `RB_CH_OFFSET`, `RBD_PAIR_OFFSET`, `RBP_SET_OFFSET`; per-channel
  `RB[%d:%d][UL|DL] buff_rgn {addr,offset,size}`; and the T0 strings verbatim:
  `SBD_IPC {shmem_base, shmem_size}`, `SBD_IPC {version, num_channels, rbps_offset}`,
  `RB_DESC[..][UL/DL](offset) = {id, ch, dir} {sbdv_offset, rb_len} {buff_size, payload_offset}`.
  → header + channel-table + ring-pair-descriptor + buffer-region model, all in shmem.
- **Boot/handshake state machine** (`cmd_*` handlers + `s5000ap_*` ops):
  CP→AP commands `INIT_START`, `CP_START`, `CP_CRASH_RESET/ACK/EXIT`; AP→CP `PIF_INIT_DONE`,
  `INIT_END`, `CMD_KERNEL_PANIC`; iod gates `rild_ready` (needs FMT iod), `No RFS io_device`;
  modemctl ops `s5000ap_on/off/boot_on/boot_off/boot_done/boot_confirm/reset/shutdown/dump_start`,
  `cp_wdt` + `cp_wdt_handler`, `cp_boot_done`; `modem_state` names
  OFFLINE/CRASH_RESET/CRASH_EXIT/NV_REBUILDING/LOADER_DONE/SIM_ATTACH/SIM_DETACH/WDT_RESET.
  `ap2cp_cfg_addr` is ioremapped and verified ("AP2CP_CFG setting is not correct",
  "CP aleady Init, Just reset release!") in `s5000ap_init_modemctl_device` / `init_mailbox_regs`.
- **CP boot is via SMC, not request_firmware** (fact): `SMC: CP_BOOT_REQ_CP_RAM_LOGGING` +
  `SMC: return_value=%d` in modem_v1; **no modem firmware filename appears anywhere in kernel
  strings** (grep `\.bin|\.img|firmware` ∩ modem terms → only bcmdhd/touchscreen hits; dead end
  recorded). The kernel instead validates a **memory-map table inside the CP binary**:
  `cp_boot_offset`, "CP memory map on CP binary", "CP memory map verification: PASS!/FAIL!",
  `table_id_ver`, `bin_count`, `cp dram_size`, `ext_mem_size`, `IPC/VSS/VSS PARAM/LOG/L2B` pairs —
  compared against the DT `shmem,*` props (shm_ipc.c). → CP image load is done by sboot/secure
  world; AP kernel only maps + verifies (inference, strongly supported).
- **Chardevs (U4 partial)**: every io_device gets a **misc device** (`sipc5_init_io_device` →
  `misc_register`, "misc_register failed") — name = iod name from DT (inference; the iod names
  `umts_ipc0/1`, `umts_rfs0`, `rmnet*`… are the strings). Plus `memshare` misc devices
  `ramdump_%s` (shm_ipc.c) and a "packet log device". Net interface: rmnet netdevices
  (`alloc_netdev`/`register_netdev` in the io_device path).
- **ioctl vocabulary** (fact, one string per cmd): IOCTL_MODEM_ON/OFF/RESET/BOOT_ON/BOOT_OFF/
  BOOT_DONE/DL_START/FW_UPDATE/FORCE_CRASH_EXIT/DUMP_START/RAMDUMP_START/DUMP_UPDATE/RAMDUMP_STOP/
  CP_UPLOAD/PROTOCOL_SUSPEND/PROTOCOL_RESUME/SHMEM_FULL_DUMP/DATABUF_FULL_DUMP/VSS_FULL_DUMP/
  ACPM_FULL_DUMP/CPLOG_FULL_DUMP/SET_TX_LINK/SECURITY_REQUEST/CRASH_REASON/AIRPLANE_MODE.
- `modem_probe` contains the board token **`eur_open`** (kernel.strings.txt:124364) — the EUR OPEN
  SKU variant of the mif pdata, consistent with the G970FXXUHHXJ6 dump.
- QoS/perf path: `shmem_qos_{cpu,mif,int}_req_handler`, `Lock/Unlock MIF|CL0|CL1|INT`,
  `exynos_devfreq_parse_ect`, and "cp requests pcie l1ss disable/enable" (CP→AP PCIe L1 sub-state
  requests — over shmem, not PCIe config; inference).

### 2026-10-12 — T3: `dumps/hhxj6/src/` ships NO modem source (dead end) — but the kernel config is extractable and settles the built-ins

- `src/` full listing (25 files, 19 065 lines): `9820_cal_data.c`, `9820_cmucal-sfr.c`,
  `9820_cpufreqcal-sfr.c`, `acpm.c`, `acpm.{h}`, `acpm_dvfs.{c,h}`, `acpm_ipc.c`, `acpm_ipc.h`,
  `acpm_ipc_ctrl.h`, `acpm_mfd.c`, `cal-if.{c,h}`, `cmucal.h`, `common.h`, `exynos-acme.c`,
  `exynos-dm.{c,h}`, `framework.h`, `fvmap.{c,h}`, `mailbox.h`, `Makefile`, `soc.Makefile`,
  `vclk.h`. **No modem/mif/sbd/dit source.** It is the CMU-calibration/ACPM/DM family only.
- **4 of those files are 14-byte `404: Not Found` mirror stubs**: `9820_cpufreqcal-sfr.c`,
  `common.h`, `mailbox.h`, `Makefile` (VERIFIED: `cat`). The src/ tree is a partial web mirror —
  treat its gaps as unknown, not absent.
- `soc.Makefile:57-60` (fact): `obj-$(CONFIG_SEC_SIPC_MODEM_IF) += pmu-cp.o` under
  `ifneq ($(CONFIG_CP_PMUCAL), y)` — a `pmu-cp.c` exists in the vendor tree but is NOT shipped.
- **NEW TOOLING RESULT: the shipped Image carries IKCONFIG.** `IKCFG_ST` at file
  `0x109d4f0`, gzip payload → 174 792-byte config at `/tmp/opencode/modem-wwan/kernel.config`
  (2063 `CONFIG_` lines). (First attempt with raw `zlib.decompress` failed — payload is gzip;
  `zlib.decompressobj(16+MAX_WBITS)` works.)
- **Modem-relevant built-ins, all `=y` (VERIFIED from extracted config)**:
  `CONFIG_MCU_IPC=y`, `CONFIG_SEC_SIPC_MODEM_IF=y`, `CONFIG_SEC_MODEM_S5000AP=y`,
  `CONFIG_UMTS_MODEM_S5000AP=y`, `CONFIG_CP_DIT=y`, `CONFIG_CP_ZEROCOPY=y`, `CONFIG_CP_PKTPROC=y`,
  `CONFIG_CP_LINEAR_WA=y`, `CONFIG_CP_SECURE_BOOT=y`, `CONFIG_CP_RAM_LOGGING=y`,
  `CONFIG_CP_ETDAC_OTP_WA=y`, `CONFIG_MODEM_IF_QOS=y`, `CONFIG_MODEM_IF_NET_GRO=y`.
  → the whole mif/SBD/DIT stack is **built into the Image**, no loadable modules.
- **No UICC/smartcard driver in the config** (grep `uicc|smartcard|sim_` → nothing modem-related).
  Inference: the SIM is reached through the CP (RIL), not an AP-side kernel UICC driver.
  (Fact: absence in this config only.)
- The project name in Kconfig is **S5000AP** (`SEC_MODEM_S5000AP`, `UMTS_MODEM_S5000AP`,
  `mif,name = "s5000ap"`) while the pinctrl/board names say **s5100** — AP-side driver family name
  vs chip name (inference; see U2 note).
- `src/acpm_ipc.c` (shipped) shows the sibling Exynos coprocessor-IPC idiom: per-channel
  front/rear ring regs + `INTCR1`/`INTGR1` interrupt-control/generate regs
  (acpm_ipc.c:294-331). Analogous to `mcu_ipc` ("mbox dump", "Reset Mailbox registers" strings)
  but NOT the modem contract — inference, supporting only.
- **Consequence for U3**: the vendor mif source is not in the dump; the contract must be pinned
  from (a) the DT (T1 — done) and (b) kernel strings (T2 — done as far as names/fields go).
  Exact struct offsets (SBD header size, RB_DESC field widths) stay UNVERIFIED — they need the
  vendor kernel source (mirror pass: `drivers/misc/modem_v1/include/sbd.h` from Samsung's
  opensource release for G970F XXU7/XXU8 kernel).

### 2026-10-12 — T4: U5 CLOSED — CP firmware = `modem.bin` on the RADIO partition; boot daemon = `cbd`; `cm.bin` is NOT modem

- **PIT (41 entries, stride 0x84, 4096-byte units — method per `notes/pit-geometry.md`; parsed
  2026-10-12 with python `struct` on `dumps/hhxj6/BEYOND0LTE_EUR_OPEN.pit`)** — modem-relevant rows:
  - `[25] RADIO  file=modem.bin  vid=16 start=57856 len=12800` → **the CP firmware partition**,
    12800×4096 = 52 428 800 bytes. (No partition is literally named "cp"/"modem" in the PIT —
    the name is RADIO; the phone's by-name link is `radio` (init.baseband.rc symlink, below).)
  - `[39] CP_DEBUG file=modem_debug.bin vid=30 start=2149888 len=1280` (5 242 880 B).
  - `[7] CPEFS vid=1 start=6 len=1536` — no flash file (not reflashed by ODIN). NOTE: all 41 PIT
    entries carry dtype=8 (re-parsed 2026-10-12), and `vid` is NOT the LUN — BOTA0 vid=1 is `sda1`
    while CPEFS vid=1 is `sdd1` (`notes/s10e-partitions.md:9-10`, phone by-name links). The
    separate-container fact for cpefs comes from the phone, not the PIT
    (consistent with `notes/dock-desktop.md:545-549`).
  - `[6] CM file=cm.bin vid=91 start=0 len=1792` — same vid-space as BOOTLOADER(80)/ECT(100)/
    ACPM(101)/BOOTLOADER2(90)/DDI(92) (the high-vid bootloader group; LUN identity not in the PIT).
  - `[2] ACPM file=acpm.bin vid=101 dtype=8 start=129 len=35` — the ACPM coprocessor firmware
    (relevant: `IOCTL_ACPM_FULL_DUMP`, `acpm_base/acpm_size` in shm_ipc strings).
- **`cm.bin` is NOT the modem firmware** (re-confirmed): 7 340 032 B = exactly 1792×4096 (the PIT
  CM length); first bytes `1e ab ad 10 00 90 21 00 …`; strings carry `CryptoManagerV50` and
  `7745557 12417ab 3598f3a 63da7d7 30e5e8b exynos9820 swp 2022-07-01 17:40 KST clang 3.8.0`.
  Already recorded as TEE in `notes/cpu-driver-9820.md:228-229`. (Command:
  `xxd cm.bin | head`; `strings -a -n 6 cm.bin`.)
- **No modem files under /vendor/firmware at all** (fact: 67 regular files + 2 subdirs (`nfc`,
  `score`) — 69 entries total, count corrected in second review — listed via
  `debugfs -R 'ls -l /firmware' vendor/vendor.raw` — bcmdhd/bcm4375 WiFi-BT, audio
  SLSI, cs35l40/cs47l93, camera fimc_is, NPU, nfc/sec_s3nrn82). The CP firmware lives **only on
  the RADIO partition**.
  - **Near-miss corrected (2026-10-12 review)**: `calliope_sram.bin` 50 772,
    `calliope_sram_2.bin` 52 300, `calliope_dram.bin` 2 253 664, `calliope_dram_2.bin` 227 344,
    `calliope2.dt` 7 620 are the **ABOX audio DSP** images, NOT modem: the kernel's `CALLIOPE`
    string sits in the `samsung,abox-*` table (kernel.strings.txt:51655-51672) with sysfs
    `calliope_version/debug/cmd` and `abox_request_firmware`/`abox_download_extra_firmware`
    (176 140-176 191). (abox does carry an `abox_modem_notifier` — audio path only.)
- **Userspace boot path (U4/U5, from `/vendor/etc/init/init.baseband.rc` via
  `debugfs -R cat`)**:
  - `on init: symlink /dev/block/platform/13d60000.ufs/by-name/radio /dev/mbin0` — the RADIO
    partition is exposed as **/dev/mbin0**.
  - `service cpboot-daemon /vendor/bin/cbd -d -tss310 -bm -mm -P platform/13d60000.ufs/by-name/radio`
    (cbd = 436 432 B in /vendor/bin) — **CBD boots the CP from the radio partition**.
  - `mount ext4 .../by-name/cpefs /mnt/vendor/cpefs` (radio:system 0771) — CP filesystem mount.
  - Dual SIM: `write /sys/module/modem_ctrl_s5000ap/parameters/ds_detect ${ro.vendor.multisim.simslotcount}`;
    simslotcount=2 → `persist.radio.multisim.config=dsds`; single/dual comes from
    `/mnt/vendor/efs/factory.prop`. The value itself is NOT in build.prop (dead end:
    `grep multisim /vendor/build.prop` → nothing) — UNVERIFIED whether this unit is ss or dsds;
    closes via live `getprop ro.vendor.multisim.simslotcount` (R01).
- **cbd internals** (strings of `dump /bin/cbd` to scratch): opens `/dev/umts_boot0`,
  `/dev/umts_ramdump0`, `/dev/mbin0`, `/dev/gsm_ipc0`, `/dev/gsm_boot0`, `/dev/cdma_boot0`,
  `/dev/nr_boot0`, `/dev/nr_ramdump0`, `/dev/modem_boot_spi`, `/dev/link_pm`, `/dev/ttySAC2/3`;
  flow strings `start_shannon_boot`, `CBP7.2 Modem boot`, `dpram_download_boot`,
  `boot frame write fail`, `[DLOAD] Download Completed !!!`, `IOCTL_MODEM_XMIT_BOOT`,
  `IOCTL_MODEM_BOOT_DONE`, `IOCTL_CP_ON`, `CP status RESET`, `CP_DEBUG NORESET`,
  `Reroute CP-UART path to VIA after CP Booting` (→ the `2cp_uart_sel` GPIO). gsm/nr/cdma names
  are multi-project leftovers (S10e = umts path; inference).
- **RIL chardevs (U4, strings of `/vendor/lib64/libsec-ril.so`, 4 077 344 B)**: opens
  **/dev/umts_ipc0** (×2), `/dev/umts_ipc1`, `/dev/umts_rfs0`, `/dev/umts_boot0`, `/dev/mbin0`,
  `/dev/mbin1`, `/dev/modem_br`, `/dev/ramdump_memshare`, `/dev/nr_rfs0`, `/dev/nr_boot0`,
  `/dev/tun`, `/dev/drb`, `/dev/rtc`. These are exactly the DT `iod,name` strings (T1) →
  **VERIFIED: misc chardev per io_device = /dev/<iod,name>** (kernel side: `sipc5_init_io_device`
  → `misc_register`; sysfs paths in init.baseband.rc: `/sys/devices/virtual/misc/umts_dm0`,
  `/sys/devices/virtual/misc/multipdp`).
- rild wiring: `vendor.sec.rild.libpath=/vendor/lib64/libsec-ril.so` (build.prop);
  `service ril-daemon /vendor/bin/hw/rild` + `ril-daemon1 … -c 2` (2nd SIM) in
  `/vendor/etc/init/init.vendor.rilchip.rc`; onrestart chain `cpboot-daemon`/`DIAG-daemon`.
- **Boot order model (inference, assembled from the facts above)**: sboot/secure world brings up
  CP silicon (kernel `SMC: CP_BOOT_REQ_*`, `CONFIG_CP_SECURE_BOOT=y`) → kernel mif probes
  (mif_pdata/shmem/mailbox_cp from dtbo) → `cbd` downloads the CP image from /dev/mbin0 through
  the `umts_boot0` channel (`dpram_download_boot`, `IOCTL_MODEM_XMIT_BOOT`) → CP sends
  `INIT_START` → AP `PIF_INIT_DONE` → `CP_START` → `INIT_END` → `rild_ready` → RIL on
  /dev/umts_ipc0/1. The exact split between sboot-side CP bring-up and cbd download is NOT pinned
  by this dump (mirror pass: sboot/CM source or Samsung kernel docs).
- Dead end: `/dev/link_pm` + `/sys/class/sec/linkpm/*` (cbd strings) have **no matching driver in
  this kernel** (grep `link_pm|linkpm` in kernel strings → only ACPI `LINK_PME` noise) — leftover
  from older projects.

### 2026-10-12 — T5: U2 — dump evidence read as "discrete Shannon S5100" — **SUPERSEDED by T7: the S10e modem is the 9820's integrated CP; the items below are family-wide (S10 5G) leftovers**

Facts pointing one way (all cited in T1/T4 above):
1. **Board-level GPIOs between AP and CP**: `ap2cp_nreset_n` (gph1-4), `ap2cp_cp_pwr_on`
   (gpm20-0), `ap2cp_dump_noti` (gpm25-0), `2cp_uart_sel` (gpm26-0), `cp2ap_cp_ps_hold`
   (gpm31-0), `cp2ap_phone_active` (gpm28-0), `cp2ap_wake_up` (gpm30-0), `ap2cp_wakeup`
   (gpm17-0), `ap2cp_pda_active` (gph1-3) — base.dtb pinctrl states (T1). A hard reset pin, a
   power-on pin, a power-save hold, and a **UART multiplexer select** are package-to-package
   signals; an in-SoC block needs none of them.
2. **PCIe doorbell + MSI window**: `pcie_1@13120000` is the S5100 doorbell but `status="disabled"`
   on this board (`notes/m12-wifi-chip.md:88`); rmem `s5100_msi 0xD9800000+0x1000`
   (`Exynos9820.h:155-164`) — an MSI message buffer for a PCIe-attached chip. The active link is
   `mif,link_name = "shmem"` (`mif,link_types = <0x200>`), with runtime link switching
   (`IOCTL_MODEM_SET_TX_LINK`, "change link: %d -> %d") — two physical link options = external chip.
3. **CP memory is downloaded through the AP**: kernel `cp dram_size`/`ext_mem_size`/
   `cp_boot_offset` + "CP memory map on CP binary" (shm_ipc.c) and cbd `dpram_download_boot`
   (T4) + `modem_spare`/`modem_if2`/`cp2cp_shm` DRAM carve-outs (0xD0000000..0xD9900000, inside
   the DRAM range based at 0x80000000 — fact from base.dtb rmem addrs).
4. **Naming split**: chip = `s5100` (pinctrl, `s5100_msi`); AP-side driver project = `s5000ap`
   (`mif,name`, `modem_ctrl_s5000ap`, `CONFIG_SEC_MODEM_S5000AP`) — driver family vs chip part
   number (inference).
- **What the dump cannot settle**: whether the 9820 silicon contains an integrated Shannon block
  at all (no datasheet in-dump). Mirror pass must check: Exynos 9820 datasheet/TRM feature list
  (modem integrated or not), S10e board photos/schematics (Shannon S5100 marking near the AP),
  Shannon S5100 product brief (PCIe + shared-memory bus interface).

### 2026-10-12 — T6: mirror pass opens — vendor `modem_v1` + `mcu_ipc` SOURCE is publicly mirrored (U3 closes from here)

- **Branches verified live** (`git ls-remote --heads`, 2026-10-12):
  - `github.com/CruelKernel/samsung-exynos9820` — branch **`cruel-HVJ5-v5.3`** exists (HEAD
    `41aba776` 2023-09-04). Sparse clone at `/tmp/opencode/modem-wwan/cruel`.
  - `github.com/LineageOS/android_kernel_samsung_exynos9820` — branches `lineage-19.1`
    (Android 12 generation, matches our HXJ6 dump) and 20/21/22.x/23.x exist. Sparse clone at
    `/tmp/opencode/modem-wwan/lineage` (branch `lineage-19.1`).
  - `github.com/GoRhanHee/android_kernel_samsung_exynos9820` — branches `gorhanhee`, `stock`,
    `upstream` exist (not cloned; redundant).
- **File inventory (fact, `find cruel/drivers/misc`)**: `modem_v1/` ships 34 files incl.
  `include/sbd.h`, `include/sipc5.h`, `include/link_device_memory_config.h`, `modem_io_device.c`,
  `modem_main.c`, `modem_ctrl_s5000ap.c`, `modem_ctrl_s5100.c`, `modem_ctrl_ss310ap.c`,
  `link_device_shmem.c`, `link_device_memory_sbd.c`, `link_device_memory_main.c`,
  `link_device_pcie.c`, `s5100_pcie.c`, `link_rx_dit.c/.h`, `link_rx_pktproc.c/.h`,
  `link_rx_zerocopy.c`, `boot_device_spi.c`, `link_ctrlmsg_iosm.c`. `mcu_ipc/` ships
  `mcu_ipc.c/.h`, `regs-mcu_ipc.h`, `shm_ipc.c`. → every T2 string maps to a readable source file.
- Convention for this section: citations `cruel:<path>:<lines>` =
  `CruelKernel/samsung-exynos9820@41aba776 (branch cruel-HVJ5-v5.3)`; `lineage:<path>:<lines>` =
  `LineageOS/android_kernel_samsung_exynos9820 (branch lineage-19.1)`. Cross-checked where noted.

### 2026-10-12 — T6b: U3 struct contract — SBD/SIPC5/mailbox/pktproc layouts pinned from vendor source (mirror)

All citations `cruel:<file>:<lines>` under `drivers/misc/` unless noted. (Branch
`cruel-HVJ5-v5.3` = One UI 5.1 vendor kernel; our dump is HXJ6/One UI 4.1 — same driver family;
lineage-19.1 clone kept for cross-checks.)

**SBD = MIPI LLI IPC** (fact): `sbd.h:18-24` — "shared buffer descriptor (SBD) architecture
**designed by MIPI Alliance**"; `sbd.h:85` "i.e. IPC Channel Descriptor Structure in
**MIPI LLI_IPC_AN**". So the AP↔CP link layer is the published MIPI Low-Layer-Interface IPC
model; Samsung's SIPC5 is the frame format on top.

**shmem region map** (`sbd.h:60-73`): CMD_DESC_RGN at offset 0, size 64 KiB; inside it
CTRL_RGN 0..1K, CMD_RGN 1K..2K, DESC_RGN 2K..62K; BUFF_RGN starts at 64K. The AP driver
allocates desc/buff regions at runtime (`desc_alloc`/`buff_alloc`, `setup_desc_rgn`
`link_device_memory_sbd.c:209-258`).

**SBD global descriptor** `struct __packed sbd_global_desc` (`sbd.h:143-168`):
`u32 version@0, u32 num_channels@4, u32 rbps_offset@8`, then
`rb_ch[MAX_LINK_CHANNELS=32]` (16 B each: `ul_rbd_offset, ul_sbdv_offset, dl_rbd_offset,
dl_sbdv_offset`, all u32, `sbd.h:133-138`), then `rb_desc[32][2]` (20 B each, below).
`MAX_LINK_CHANNELS=32`, `MAX_SIPC_CHANNELS=256` (`modem_v1.h:80-81`).

**RB_DESC** `struct __packed sbd_rb_desc` (20 B, `sbd.h:87-128`):
`u16 ch@0; u16 reserved@2; u16 direction@4 (0=UL/AP→CP,1=DL/CP→AP); u16 signaling@6
(0=polling,1=interrupt); u32 sig_mask@8; u16 length@12 (ring entries); u16 id@14 (link id =
index into RBP arrays); u16 buff_size@16 (default 2048); u16 payload_offset@18`.
AP fills `sig_mask = MASK_INT_VALID|MASK_SEND_DATA = 0x0081` (`setup_sbd_rb_desc`,
`link_device_memory_sbd.c:105-118`; masks `link_device_memory.h:147-160`) **and always
`signaling = 1` (interrupt) there — the vendor never exercises the polling path; a
signaling=0 link is our construction (second review, design P4).**

**RBP set** (ring pointers): one shared array of u16 at `g_desc->rbps_offset`, laid out
`[ul_rp[]][ul_wp[]][dl_rp[]][dl_wp[]]`, each `num_channels` long
(`link_device_memory_sbd.c:255-258`; `rb->rp = &sl->rp[dir][rb->id]` :182-183). **A ring post
is just: copy payload into `buff[wp]`, write `size_v[wp]` (u32; low16=len, and for
packet-switched channels high bits carry the ch id — `sbd_pio_tx`
`link_device_memory_sbd.c:541-592`), then `*wp = circ_new_ptr(len, wp, 1)` + `smp_mb()`.**
SBDV vectors = `addr_v[]` (u32 shmem-offset of each buffer, pre-registered at setup,
`setup_sbd_rb` :186-193) + `size_v[]` (u32 written per post).

**Doorbell (AP→CP)**: after posting, `send_ipc_irq(mld, mask2int(mask))`
(`link_device_shmem.c:1259+`, `link_device_memory.h:858-863`) → `send_ap2cp_irq`
(`link_device_shmem.c:2903-2907`): `mbox_set_value(MCU_CP, mbx_ap2cp_msg=0, mask)` then
`mbox_set_interrupt(MCU_CP, int_ap2cp_msg=0)`. Message-word encoding
(`link_device_memory.h:147-175`): bit7 `MASK_INT_VALID`, bit6 `MASK_CMD_VALID`, bits5:0 CMD
(`CMD_INIT_START=1 … CMD_PIF_INIT_DONE=0x0D`, :162+); data-post masks SEND_DATA=0x1,
SEND_FMT=0x2, REQ_ACK_FMT/RAW 0x20/0x10, RES_ACK_FMT/RAW 0x08/0x04 (pairing corrected in
second review — `link_device_memory.h:153-156`).

**MCU_IPC mailbox regs** (the `mailbox_cp` node, 0x15920000+0x180):
`MCUCTLR@0x0, INTGR0@0x8, INTCR0@0xc, INTMR0@0x10, INTSR0@0x14, INTMSR0@0x18, INTGR1@0x1c,
INTCR1@0x20, INTMR1@0x24, INTSR1@0x28, INTMSR1@0x2c, ISSR0..3@0x80..0x8c`
(`mcu_ipc/regs-mcu_ipc.h:18-32`). **Mailbox value slots = `ISSR0 + 4*mbx_num`, up to 64 slots**
(`mbox_get/set_value`, `mcu_ipc/mcu_ipc.c:257-271`); **interrupt generate = write
`1<<int_num` (int_num<16) to `INTGR1`** (`mcu_ipc.c:238-244`); CP→AP interrupts arrive on the
node's SPI 41, status read from `INTSR0 & 0xFFFF0000`, cleared by writing back to `INTCR0`
(`mcu_ipc.c:47-85`). Compatible `samsung,exynos-shd-ipc-mailbox` bound at
`mcu_ipc.c:529-539`, `devm_ioremap_resource` :464. So the T1 mailbox ids map to:
ISSR0=mbx_ap2cp_msg, ISSR1=mbx_cp2ap_msg, ISSR2=united_status(ap), ISSR3=united_status(cp)
(per `mif,*_mbx_*` DT ids, T1) — **the SBI "united status" word is a mailbox register, not
shmem** (fact: `mbox_update_value(MCU_CP, ap_status…)` `link_device_shmem.c:401`).

**SIPC5 link header** (`include/sipc5.h:22-61`): `u8 cfg@0` (top 5 bits must be 0b11111 —
`SIPC5_START_MASK 0xF8`; bit2 PADDING, bit1 EXT_FIELD, bit0 CTL_FIELD), `u8 ch@1` (= SIPC
channel id), `u16 len@2` (header+payload), optional `u8 ctrl@4` (multi-frame: `id:7,more:1`,
`modem_v1.h:213-216`) or `u16 ext_len@4` (frame >64K). Header 4/5/6 B. Padding to 8 B when
`ld->aligned` (`sipc5_calc_padding_size`, sipc5.h:93-98 — `BITS_PER_LONG/8` = 8 on arm64).
`iod->id` written into ch (`sipc5_build_header`, `modem_io_device.c:1495-1521`).
**SIPC channel ids** (`modem_v1.h:128-210`) match the T1 DT exactly: PDP_0..14 = 10..24
(rmnet0..7 = 0xa..0x11 ✓), CIQ=26 ✓, CASS=35 ✓, BOOT_0..9=215..224 (umts_boot0 0xd7=215 ✓),
DUMP=225..234 (umts_ramdump0 0xe1=225 ✓), FMT_0..9=235..244 (**umts_ipc0/1 = 0xeb/0xec =
FMT_0/1** ✓), RFS_0..9=245..254 (umts_rfs0 0xf5=245 ✓).

**write() → ring post, payload opaque (settles the RIL question)**: `misc_write`
(`modem_io_device.c:989-1110`) = `copy_from_user` **verbatim** (:1063), prepend SIPC5 header,
pad, split at `iod->max_tx_size`, `ld->send`. The kernel NEVER parses the RIL request bytes —
it only frames (SIPC5) and routes (ch id = iod->id). Same for the rmnet netdev path
(`io_dev_send_to_link_dev` :1280-1380: pulls the eth header only in handover mode). → **the
umts_ipc0 payload protocol is defined solely by closed userspace (`libsec-ril.so`) ↔ closed CP
firmware**; the kernel contract above is the whole open part. (`misc_register(&iod->miscdev)`
with `miscdev.name = iod->name` at :1550-1555 — T4's /dev/<iod,name> now VERIFIED in source.)

**shmem physical base (T1 inference now VERIFIED in source)**: `mld->base =
shm_get_ipc_region()` = `pdata.p_addr + pdata.ipc_off` (`link_device_shmem.c:3963-3964`;
`shm_ipc.c:509-511` — offset corrected in second review) = **0xF0000000 + 0xB200000 = 0xFB200000**, size 0x500000; passed to
`create_sbd_link_device(ld, …, mld->base, mld->size)` (:4004-4006). Boot region = cp+vss
(0xF0000000..0xF8000000), VSS = 0xF7800000.

**pktproc (zerocopy RX, `CONFIG_CP_PKTPROC=y` + `CONFIG_CP_LINEAR_WA=y` — both =y in our
config)**: global ring descriptor `struct pktproc_global_desc {u32 desc_base_addr,
num_of_desc, data_base_addr, fore_ptr, rear_ptr}` at **`modem_spare` phys 0xD0000000**
(`shm_get_pktproc_desc_region` under `CONFIG_CP_LINEAR_WA` → `p_spare_addr`, `shm_ipc.c:535-543`);
AP writes `desc_base_addr = 0x50000000|0x100`, `data_base_addr = 0x50000000|0x100000`,
`num_of_desc = 8192` (`link_rx_pktproc.c:133-147`; `link_rx_pktproc.h:19-25`). Per-desc 20 B:
`{u32 data_addr; u32 information; u32 rsv1; u16 rsv2; u16 filter_result; u16 length; u8
channel_id; u8 rsv3; u32 rsv4}` (`link_rx_pktproc.h:37-46`). Source comment `shm_ipc.c:262-268`
maps the **CP-side view 0x5000_0000..0x515F_FFFF ↔ AP `modem_spare` 0xD000_0000+0x1600000**
(desc 1 MiB, data 16 MiB, ZMB 5 MiB) → **0x50000000 is the CP's alias window for AP DRAM at
0xD0000000** (inference from that comment; sizes arithmetic VERIFIED).

**DIT register map** (inside the 0x1A2A0000+0x10000 window; `link_rx_dit.h:74-102`):
`DIT_SW_COMMAND@0x0, DIT_CLK_GT_OFF@0x4, DIT_DMA_INIT_DATA@0x8, TX_DESC_CTRL@0xC,
TX_HEAD_CTRL@0x10, TX_MOD_HD_CTRL@0x14, TX_PKT_CTRL@0x18, TX_CHKSUM_CTRL@0x1C,
RX_DESC_CTRL@0x20, RX_HEAD_CTRL@0x24, RX_MOD_HD_CTRL@0x28, RX_PKT_CTRL@0x2C,
RX_CHKSUM_CTRL@0x30, DMA_CHKSUM_OFF@0x34, SIZE_UPDATE_OFF@0x38, TX_RING_ST_ADDR_0/1@0x3C/0x40,
RX_RING_ST_ADDR_0/1@0x44/0x48, INT_ENABLE@0x4C, INT_MASK@0x50, INT_PENDING@0x54,
DIT_STATUS@0x58, DIT_BUS_ERROR@0x5C`; CLAT filter regs @0x2000+, NAT filter/desc regs @0x4000+
(:110-156). DIT = HW classifier doing NAT/CLAT offload + per-flow steering into rmnet rings
(T2 strings + these regs).

### 2026-10-12 — T7: s5000ap controller + boot state machine pinned; **U2 CORRECTED — the S10e modem is the 9820's INTEGRATED CP, not a discrete S5100**

**U2 — the silicon question, settled from vendor source (this REPLACES the T5 verdict):**
- Samsung's own DT comments (`cruel:arch/arm64/boot/dts/exynos/exynos9820-rmem_s5100_dual.dtsi`):
  line 21 `/* External CP - S5100 */` over `modem_if2 0xD1600000+0x4200000` ("66MB: VSS 2MB,
  SHM MEM 64MB"), line 28 `/* CP2CP shared memory - 64MB */`, line 34 `s5100_msi`
  `compatible="exynos,s5100-msi"`, and line 40 **`/* Internal CP - S500AP */`** over
  `modem_if 0xF0000000+0xB900000`. So the 9820 has an **integrated CP** (Samsung project name
  S500AP/S5000AP) AND the family supports an **external S5100 companion** (Exynos Modem 5100).
- **Which one the S10e uses**: `cruel:arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi:15`
  includes **only `modem-s5000ap-sipc-pdata.dtsi`** (internal, link shmem). beyond1lte likewise.
  The S10 5G board `exynos9820-beyondx_common.dtsi` includes **both** `modem-s5100-dual-sipc-pdata.dtsi`
  (link `0x800 LINKDEV_PCIE`, `mif,name="s5100"`, `mif,modem_type=<12>`) and
  `modem-s5000ap-dual-sipc-pdata.dtsi`.
- **Shipped kernel config (kernel.config, T3 extraction)**: `CONFIG_SEC_MODEM_S5000AP=y`,
  `# CONFIG_SEC_MODEM_S5100 is not set`, `# CONFIG_LINK_DEVICE_PCIE is not set`,
  `# CONFIG_SEC_SIPC_DUAL_MODEM_IF is not set` — the S5100/PCIe link code is NOT in our Image.
- **Live phone** (`notes/s10e-live-reads.md:125-127`): `/reserved-memory` children include
  `modem_if, modem_spare, cp_ram_logging` but **no `modem_if2`, no `cp2cp_shm`, no `s5100_msi`** —
  the external-modem carve-outs are not reserved on this device.
- **Marketing confirmation**: Samsung tech blog "Exynos 9820: Delivering groundbreaking features
  on the Galaxy S10" — "the Exynos 9820's LTE-Advanced Pro modem. **The integrated modem**
  supports LTE Category 20 … also supports dual SIM"
  (https://semiconductor.samsung.com/news-events/tech-blog/exynos-9820-delivering-groundbreaking-features-on-the-galaxy-s10/
  , fetched 2026-10-12).
- **T5 re-read**: its "discrete chip" evidence was family-wide leftovers in the shared SoC dtsi —
  the `s5100_evt1_dual_*` pinctrl states are DEFINED in `exynos9820-pinctrl.dtsi` but referenced
  ONLY by `modem-s5100_evt1-dual-sipc-pdata.dtsi:29-37` (external-CP node, not included by
  beyond0lte); `modem-s5000ap-sipc-pdata.dtsi` has `pinctrl-names` but **no pinctrl-0**. The
  `2cp_uart_sel`/`ap2cp_nreset_n`/`cp_pwr_on` pins belong to the external-S5100 option.
  `pcie_1` being the "S5100 doorbell" (m12 note) is consistent — it's the external-chip link,
  absent on S10e. **Verdict: S10e cellular = Exynos 9820 INTEGRATED Shannon CP; "S5100" names in
  the dump are 5G-variant leftovers. U2 CLOSED.** (Consequence: no board power/reset sequencing
  for the active modem — CP control is via PMU regs + SMC, below.)

**Vendor dtsi = the exact source of our dtbo fragment B** (fact):
`cruel:arch/arm64/boot/dts/exynos/modem-s5000ap-sipc-pdata.dtsi` matches the T1 dump property for
property: `reg = <0 0x10000 0>, <0 0x1A2A0000 0x10000>, <0 0x1A220000 0x2000>` (comment: "modem
binary start offset, DIT, SYSREG_BUSC" — settles the "offset" reg-name: **modem main binary start
offset**), 4 IRQs `<0 375 0>,<0 68 0>,<0 67 0>,<0 66 0>` (comment INTREQ__CP2AP_WAKEUP /
DIT_BusErr / DIT_RxEnd / DIT_TxEnd), `ap2cp_cfg_addr=<0x15020000>`, 22 iodevs incl.
`ipc_loopback0`, `mif,link_attrs=<0x7C9>` decoded in-comment: XMIT_BTDLR 0x400 | DUMP_ALIGNED
0x200 | BOOT_ALIGNED 0x100 | MEM_DUMP 0x80 | MEM_BOOT 0x40 | DPRAM_MAGIC 0x08 | SBD_IPC 0x01.
(T1's "umts_ipc0 10×0x1000 UL" was a hex misread: `0x10`=16 buffers × 4096 — matches dtsi.)
**U7 CLOSED (DT half)**: `gpios = <&gpg3 4 0>,<&gpg3 3 0>,<&gpg3 2 0>,<&gpg3 1 0>,
<&gpg1 5 0>,<&gpg1 4 0>,<&gpg1 3 0>,<&gpg1 2 0>` with comment "HW revision : CP, AP /
XGPIO27-CP_REV0..XGPIO24-CP_REV3 / XGPIO13-HW_REV0..XGPIO10-HW_REV3" — **all 8 are hw-rev sense
bits** (first 4 = CP_REV0..3 on gpg3, last 4 = HW_REV0..3 on gpg1), consumed by
`get_system_rev()` (`modem_ctrl_s5000ap.c:117-141`, compiled because `# CONFIG_HW_REV_DETECT is
not set`), packed LSB-first into `hw_rev`, written to mailbox field `sbi_sys_rev` (0xff@6) of
`mbx_ap2cp_united_status` (:254-255). NOT handshake pins — T2's 4+4 inference is dead.
Dual-SIM detect = module param `ds_detect` (`# CONFIG_GPIO_DS_DETECT is not set`; sysfs
`/sys/module/modem_ctrl_s5000ap/parameters/ds_detect`, `ds_detect_store` :169-187 →
`sbi_ds_det` 0x3@0xe) — matches init.baseband.rc write (T4).

**CP control = PMU regs via SMC (new constants, all pinned)**:
- SMC function id **`0x82000700`**, sub-ops `READ_CTRL=3`, `WRITE_CTRL=4`
  (`cruel:drivers/soc/samsung/cal-if/pmucal_cp.h:15-17`; calls `pmucal_cp.c:229-298`).
- 9820 sequences (`cruel:drivers/soc/samsung/cal-if/exynos9820/pmucal_cp_exynos9820.h:2-25`),
  all on PMU base **0x15860000** (= `EXYNOS_PMU_BASE`, already VERIFIED in
  `Exynos9820.h:273`): `CP_CTRL_S @+0x3014` set bit3 (cp_init step 1);
  `CP_CONFIGURATION @+0x3000` bit0 = CP reset (1=release, 0=assert);
  `CP_STATUS @+0x3004` bit0 = CP initialized (status read + waits);
  `CP_IN @+0x3024` bit4 = CP ready (wait after init/release);
  `CP_OUT @+0x3020` bit16 = dump_pc_no_pg toggle.
  `cal_cp_init/status/reset_assert/reset_release` = these sequences
  (`dumps/hhxj6/src/cal-if.h:98-105` declares them; impl `pmucal_cp.c` in mirror).
- `AP2CP_CFG` = plain MMIO write of **1** to **0x15020000** (ioremap SZ_64,
  `modem_ctrl_s5000ap.c:705-713`; `s5000ap_on` :333-342 writes 1, reads back, expects 1) —
  used **instead of** `cal_cp_init` when `CONFIG_CP_SECURE_BOOT=y` (our config: =y): the
  secure-boot CP bring-up is owned by sboot/EL3; AP only sets AP2CP_CFG. No DT node owns
  0x15020000 (bare address property; fact: grep base.dts → no node).
- Boot state machine (cbd-driven ioctls, `modem_ctrl_s5000ap.c`):
  1. `IOCTL_MODEM_ON` → `s5000ap_on` (:302): zero all mailbox slots (`init_mailbox_regs` :203-217),
     set `sbi_pda_active`=1 in ISSR2, AP2CP_CFG=1.
  2. `IOCTL_MODEM_BOOT_ON` → `s5000ap_boot_on` (:445): `ld->boot_on` (map boot region),
     notify STATE_BOOTING, **poll `mbx_cp2ap_united_status` (ISSR3) `sbi_cp_status` (0xf@1) != 0**
     (100 × 10-20 ms), disable cp_wdt IRQ.
  3. cbd image download over `umts_boot0` (STD_UDL frames, `sipc5.h:261-297`: cmd nibble
     SEND=0x1, CRC=0xC; `std_dload_info {size,mtu,num_frames}`).
  4. CP→AP mailbox msg `CMD_INIT_START` → AP replies `CMD_PIF_INIT_DONE`; `CMD_INIT_END`
     completes `init_cmpl` (link_device_shmem cmd handlers, T2 vocabulary).
  5. `IOCTL_MODEM_BOOT_OFF` → `s5000ap_boot_off` (:479): wait `init_cmpl` (MIF_INIT_TIMEOUT),
     enable cp_wdt IRQ (SPI 375 = `platform_get_irq(pdev,0)` = "CP2AP_WAKEUP" line — so
     **CP_WDT and CP2AP_WAKEUP are the same interrupt**, :683-691), notify STATE_ONLINE.
  6. off/shutdown/reset (:357-444): `mbox_set_interrupt(MCU_CP, int_cp_wakeup=1)` then
     `cal_cp_reset_assert/release` + `mbox_sw_reset(MCU_CP)` (MCUCTLR bit0, `mcu_ipc.c:301-314`).
- `s5000ap_reset` (:404; the magic check is at :418 — corrected in second review) reads a magic at **ipc-shmem offset 0xF80** (`*(base+0xF80)==0xDEB`
  skips reset) — another shmem-resident field (DPRAM_MAGIC attr 0x08, above).

### 2026-10-12 — T8: DIT hw programming, CP-binary TOC, and the AT-question settled (no AT anywhere)

**DIT init sequence** (`cruel:drivers/misc/modem_v1/link_rx_dit.c:435-472`, `dit_init_hw`):
`DIT_DMA_INIT_DATA=0x4020`, `DIT_SW_COMMAND=1`, `DIT_INT_ENABLE=0x7`, `DIT_INT_MASK=0x7`,
`DIT_DMA_CHKSUM_OFF=0`, `DIT_SIZE_UPDATE_OFF=0`, `DIT_NAT_ZERO_CHK_OFF=0xf`,
TX: `DESC_CTRL=0x80, HEAD_CTRL=0x80, MOD_HD_CTRL=0x20, PKT_CTRL=0, CHKSUM_CTRL=0x20`;
RX: `DESC_CTRL=0xC0, HEAD_CTRL=0xC0, MOD_HD_CTRL=0x30, PKT_CTRL=0, CHKSUM_CTRL=0x30`;
`DIT_RX_RING_ST_ADDR_0/1` = `dit_dev.dma` — a **`dma_zalloc_coherent` buffer** (:732), i.e.
DIT's own DMA ring is separately allocated (also fed to `DIT_NAT_RX_DESC_ADDR_0/1` :117,337),
distinct from the pktproc gdesc ring in `modem_spare` (T6). DIT RX completion consumes
`pktproc_desc[rear_ptr].channel_id` (`link_rx_dit.c:30,154`) — DIT classifies CP→AP packets
into the pktproc ring, AP steers by `channel_id` to the right rmnet iod.
`sysreg_busc` (0x1A220000): only two writes in the driver — `+0x1700 = 1`, `+0x1704 = 6`
(:470-471) — DIT bus/interconnect control; bit semantics UNVERIFIED (no other reference).

**CP binary TOC** (`cruel:drivers/misc/mcu_ipc/shm_ipc.c:780-900`): the CP image (modem.bin)
carries a table at **offset 0xa0** ("cp memory map offset", :865): `{table_id_ver, dram_size,
ext_mem_size, ext_bin_count, sExtBin[]}` with 4-char tags **IPC / VSS / VPA / ZMC / LOG / L2B**
giving off/size per region; kernel `verify_cp_memory_map()` compares against the DT `shmem,*`
props and on PASS overrides `pdata.ipc_off` etc. from the binary (:875-900). So the shmem
partition is authoritative from BOTH sides (DT + modem.bin TOC) — a Windows loader must satisfy
one of them; the TOC is readable offline from the RADIO partition (cheap observable: parse
modem.bin header — deferred, R01-adjacent).

**AT command surface — NEGATIVE RESULT (exact queries, 2026-10-12)**:
`strings -a -n 3 /tmp/opencode/modem-wwan/libsec-ril.so | grep -E "^AT[\+CD]"` → **0 hits**;
`strings -a libsec-ril.so | grep -cE 'AT\+|ATD|ATZ'` → **0**; same greps on `cbd` → 0.
Mirror grep `AT+|ttySAC|uart` in `drivers/misc/modem_v1/*.c` → no AT path (only an unrelated
comment hit). **The RIL control protocol on /dev/umts_ipc0 is a proprietary binary request/reply
encoding defined only by the closed `libsec-ril.so` ↔ closed CP firmware** (fact: absence of AT
strings + kernel framing-only, T6). The CP UART (`ttySAC2/3` in cbd strings, `2cp_uart_sel`
GPIO) is a **debug/log console** ("Reroute CP-UART path to VIA after CP Booting"), not a
documented AT interface; whether it emits/eats AT at runtime is UNKNOWN (closes by: live
console capture, deferred by repo rule 8).

### 2026-10-12 — T9: prior-art sweep — nobody has ever driven a Shannon modem from Windows; every WoA port lists Mobile Data ❌

- **Mu-Silicium** (org moved: `Mu-Silicium/edk2-platforms` → auth prompt = gone/renamed; current
  repo **`Project-Silicium/Mu-Silicium`**, cloned at `/tmp/opencode/modem-wwan/mu`).
  - `Status.md` (raw, fetched 2026-10-12): **Exynos 9820 / Galaxy S10 `beyond1lte`**: UEFI-only
    table (Display ✅, Internal Storage ❌, Windows Boot ❌, Linux Boot ❌), state *Inactive* —
    no Windows OS-status table at all, so no WWAN row. **Exynos 9830 / S20 5G `x1s`**: WinPE
    boots; OS-status table lists **"Mobile Data ❌"**. Same ❌ in every Windows/Linux OS-status
    table in the shown portion of the file (starlte S9, gtaxllte, b5q Flip5, dm2q S23+ Linux,
    aston OnePlus 12R both OSes, peridot, bale_b, infiniti Linux; file read to ~line 1511/5650).
    ~~**No device shown has working cellular under Windows.**~~ **CORRECTED in the second-opinion
    review (2026-10-12): the full file carries `Mobile Data ✅` in the WINDOWS status tables of at
    least 5 Snapdragon phones** — cepheus (Mi 9, :3325), vayu (POCO X3 Pro, :3391),
    lisa (:4094, "SIM2 doesn't work"), surya (:4248), miatoll (:4379), each under "Needs
    Windows Drivers" (vendor IHV driver releases: qaz6750/XiaoMi9-Drivers, woa-vayu/
    POCOX3Pro-Releases, woa-miatoll/Miatoll-Releases); nuwa/starqltechn/miatoll's other ✅ rows
    are Linux tables. The Exynos/Shannon-specific claim stands — every ✅ device is a Qualcomm
    modem — and the corrected fact SUPPORTS the design's architecture: the IHV lower-edge shape
    demonstrably carries cellular under Windows on phones.
  - `S5E9820Pkg` contents (`git ls-tree`): only `GpioBankLib`, `PlatformSecLib`, `PmicLib`,
    `ResetSystemLib`, `UsiLib` + `S5E9820Pkg.dsc.inc` — **zero modem/WWAN code** (tree grep
    `modem|wwan|cellular|shannon|ril` → only OpenSSL `ecp_nistz256` filename noise).
- **`sonic011gamer/edk2-exynos`** (master @ `c0003541`, cloned): tree grep
  `modem|wwan|cellular|shannon|ril` → **zero hits**; contents = `Exynos7420Pkg` +
  `RenegadePkg` (SD835 boot infra: KernelErrataPatcher, Reboot2Payload, SMBIOS) — no CP code.
- **`ntdevlabs/exynos9810-woa` README** (raw, fetched 2026-10-12): driver set = UFS, HSI2C,
  touch only; "Back up EFS / modem / partition table" is a *backup* step, not a driver. The
  validated 9810 base never had cellular under Windows either.
- **Web searches, exact queries + dates (all 2026-10-12, no relevant hits)**:
  - `"Shannon" modem "Windows on ARM" driver Exynos cellular Windows` → nothing on-topic
    (only a namu.wiki mention of Shannon 5153 in Exynos 2400, semiwiki book PDF).
  - `Exynos 9820 mobile data Windows WoA driver modem CP shared memory` → nothing on-topic.
  - `"Exynos Modem 5100" OR "Shannon 5100" Windows driver ARM64 WWAN` → **no results at all**.
  - `Surface Pro X Snapdragon X16 LTE modem Windows driver MBIM "wmbclass" OR "qcdxrmnet"` →
    nothing definitive (picclick noise); the MS docs answer (T10) stands on its own.
  - `Shannon modem Windows ARM` (task-suggested term) covered by the first query above.
- **Verdict (fact by exhaustive negative)**: prior-art ladder rung 5 is EMPTY for Samsung
  modems on Windows — no EDK2 package, driver, or port has ever brought up Shannon/Exynos
  cellular under Windows on any device. The only Windows cellular success stories anywhere are
  Qualcomm IHV drivers on Snapdragon WoA (see T10(d)).

### 2026-10-12 — T10: U6 CLOSED — Windows' WWAN contract (all cited to learn.microsoft.com, fetched 2026-10-12)

**(a) Architecture + inbox/vendor split** — "Cellular Architecture and Implementation"
(https://learn.microsoft.com/en-us/windows-hardware/drivers/network/cellular-architecture-and-driver-model,
updated 2025-07): user mode = **WwanSvc** (modem init/registration/power/connection + access
interfaces SAR/PCO/Scan/SMS/USSD/LTE-config/SIM-file/PIN), **WCM** (L3 + media selection), SMS
Router + SMS WinRT, LPA (eSIM), Cellular CSPs, mobile-plans service, **Cellular UX** (Settings +
flyout + PNIDUI bars + airplane-mode), COSA/MultiVariant. Kernel mode = **NDIS**, **NetAdapterCx**,
**MBBCx** (inbox WDF class extension: "handles the control OIDs from NDIS and converts them to
MBIM commands for the IHV driver"), and the **IHV driver = the lower edge**. Inbox: WwanSvc/MB
service, MBBCx, **wmbclass** (USB only), USBCCGP. Vendor: everything below the lower edge for
non-USB transports. NOTE: the task's term **"WFEM (Windows Foundation for Embedded Modems)" does
not exist in Microsoft's public docs** — exact queries `"Windows Foundation for Embedded Modems"
OR "WFEM" modem Windows drivers` and `"foundation for embedded modems" Windows` (2026-10-12) →
zero relevant results; the real names are MB driver model / MBCD / MBBCx as above.

**(b) What a non-USB modem must provide** — same page, "Windows 10 cellular implementation
requirements" (verbatim): "Implement the **MBIM protocol interface in your modem hardware**.
Implement a **USB interface** to the modem hardware. This can be a removable USB dongle or
**another interface that presents itself as a USB host controller**." And: "For **PCIe** cellular
modem devices, the IHV vendors are expected to provide an **IHV client driver that translates the
MBIM commands to be transmitted over the PCIe bus**." So the sanctioned non-USB shape =
NetAdapterCx/**MBBCx client driver carrying MBIM messages** over the vendor transport
(MbbCx APIs "for MBB-specific control path operations like MBIM message handling",
https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/mobile-broadband-mbb-wdf-class-extension-mbbcx).
NDIS-side contract: production miniport = **NdisMediumWirelessWan, EnableDhcp=0, no ARP
emulation, IfType IF_TYPE_WWANPP/2, lower "ppip"** ("MB Miniport Driver Types",
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/mb-miniport-driver-types);
data path = raw IP NBLs flagged IS_IPV4/IS_IPV6 ("MB Raw IP Packet Processing Support",
https://learn.microsoft.com/en-us/windows-hardware/drivers/network/mb-raw-ip-packet-processing-support).
**AT-based MB: legacy Windows-7-era model** ("The MB driver model is a software architecture
provided with Windows 7 and later", mb-interface-overview) — but the Windows 10 *requirements*
text mandates MBIM, and no AT-based path appears anywhere in the Win10 architecture. **Verdict:
for Windows 10/11 (incl. ARM64) treat MBIM as required; AT-only modems are not a supported
product path** (inference from the requirements text; no explicit "AT deprecated" notice found —
dead end: `Windows 10 mobile broadband "AT commands" miniport legacy MBIM required` → no results).
QMI: never mentioned in MS docs → vendor-specific, not inbox (inference from absence).

**(c) Inbox MBIM stack** — **MBCD** = "the inbox driver in Windows 8 for USB functions that
conform to the MBIM specification" (mb-interface-terms), named **wmbclass** in the architecture
page ("for USB-based modems, the interfaces are standardized and handled by the inbox wmbclass
driver"); it loads on USB composite devices via USBCCGP + Microsoft OS compatible ID **"MBIM"**
("MB Interface Model Overview", https://learn.microsoft.com/en-us/windows-hardware/drivers/network/mb-interface-model).
→ **Inbox MBIM binds USB only.** No inbox driver exists for a memory-mapped modem.

**(d) Snapdragon WoA reality** — the PCIe sentence in (b) IS the documented Snapdragon model:
Qualcomm ships the IHV lower-edge client driver; MBBCx/NDIS above it (fact from docs). The exact
Qualcomm driver package name was not confirmable from MS docs (search dead end recorded in T9) —
inference: the "Qualcomm 4G LTE/5G WWAN" drivers on Surface Pro X / WoA laptops are that IHV stack.

**(e) User-facing surface** — Win32 **Mobile Broadband API (MBN)** `mbnapi.h` (`IMbnConnection`,
`IMbnDeviceServices`…) is **deprecated: "Starting in Windows 10, version 1803, the Win32 APIs
described in this section are replaced by the Windows Runtime APIs"**
(https://learn.microsoft.com/en-us/windows/win32/api/mbnapi/nn-mbnapi-imbnconnection; portal
https://learn.microsoft.com/en-us/windows/win32/mbn/mobile-broadband-networks-portal). Current =
WinRT `Windows.Networking.NetworkOperators` (`MobileBroadbandModem` — mobile-operator apps with
privileged access) + `Windows.Networking.Connectivity.MobileBroadband` (`MobileBroadbandAccount`,
`NetworkOperator`); `wwanapi.dll` exists in-box (NirSoft listing) but has **no Learn reference**
(`https://learn.microsoft.com/en-us/windows/win32/api/wwanapi/` → 404, checked 2026-10-12).
What the user sees = Cellular UX / flyout / mobile plans (page in (a)).

**(f) DIT/NAT-offload equivalent** — Windows has **no hardware-classification requirement**: the
production MB miniport is exactly "IP packet handling capability" (one IP pipe per connection;
MBIM multiplexes connections by **CID**, the analogue of rmnet0..7/multipdp channels). CLAT/NAT
offload (DIT's job, T6/T8) is a modem-internal optimization invisible to the NDIS contract —
the miniport just presents per-CID IP. So the S10e's rmnet rings map 1:1 onto the Windows model
*if* a control plane exists to activate them.

### 2026-10-12 — T11: feasibility shape (INFERENCE from T6–T10 facts; no protocol details invented)

The **data plane is fully open** (T6: SBD ring post = memcpy + u16 wp bump; doorbell = mailbox
ISSR0/INTGR1; rmnet = raw IP frames; polling signaling legal per `sbd.h:99-102`). The **CP boot
plane is fully open** (T7: SMC 0x82000700 / PMU 0x15860000+0x3000-series / AP2CP_CFG=1 /
STD_UDL download on umts_boot0 / INIT_START→PIF_INIT_DONE→INIT_END mailbox cmds; modem.bin TOC
@0xa0 self-describes, T8). The **RIL control plane is closed** (T6/T8: kernel frames only; zero
AT strings; encoding lives in libsec-ril.so ↔ CP firmware). Candidate architectures:

1. **(i) MBBCx/NDIS WWAN miniport + MBIM shim over SIPC5** — Windows-correct shape (T10(b)),
   but requires reimplementing the *closed* SEC-RIL request encoding (SIM files, registration,
   PDN activate) inside the driver's MBIM-message handler. Cheapest observable rung: on Android,
   capture framed umts_ipc0 traffic (the driver's own pktlog device / `mif_pkt` hooks, or a TWRP
   shmem dump of the 0xFB200000 rings) and check whether request framing is structured enough to
   reverse; until then the encoding is UNKNOWN, not assumed.
2. **(ii) rmnet-as-IP-pipe skeleton first (recommended next rung)** — Windows driver that boots
   the CP (open contract), completes the mailbox init handshake, and pumps raw IP over one rmnet
   ring pair. Observable WITHOUT any closed protocol: CP reaches `sbi_cp_status != 0`, INIT_END
   arrives, and (if a PDN were pre-activated) packets flow. Proves MMIO/SMMU/interrupt plumbing
   end-to-end; leaves PDN activation (the closed part) explicitly un-solved.
3. **(iii) AT-over-CP-UART** — **documented dead end** (T8: no AT strings in libsec-ril/cbd;
   CP UART is a debug console behind `2cp_uart_sel`). Only reopenable via a live console capture
   (deferred, repo rule 8).
4. **(iv) USB-presentation per MS requirement** — the phone's own CP is not on USB; presenting it
   as USB means writing the MBIM→SEC-RIL translator anyway = (i) with extra steps. A literal USB
   MBIM *dongle* works today via inbox wmbclass but isn't this phone's cellular.

Cheapest rung overall: **(ii)** — CP boot + SBD link bring-up + mailbox handshake from Windows,
all constants already pinned (T6/T7), zero closed-protocol dependency; it converts U3's contract
into device-verified facts and isolates the closed control plane as the single remaining wall.

### 2026-10-12 — T12: constants frozen into `Exynos9820.h` + memory map (VERIFIED unless marked)

| Constant | Value | Status | Source |
|---|---|---|---|
| `EXYNOS_CP_SHMEM_BASE` | `0xF0000000` | VERIFIED | = `EXYNOS_RSVD_MODEM_IF_BASE`; dtbo `shmem` partitions it (T1) |
| `EXYNOS_CP_BOOT_SIZE` / `EXYNOS_CP_VSS_BASE` / `EXYNOS_CP_L2B_BASE` / `EXYNOS_CP_VPARAM_BASE` | `0x7800000` / `0xF7800000` / `0xF8000000` / `0xFB700000` | VERIFIED | `shmem,*_offset/size` cells, `frag_dtb02.txt` |
| `EXYNOS_CP_IPC_BASE+SIZE` | `0xFB200000` + `0x500000` | VERIFIED | `shmem,ipc_offset`; use `link_device_shmem.c:3963-3964` |
| `EXYNOS_MODEM_DIT_BASE+SIZE` | `0x1A2A0000` + `0x10000` | VERIFIED | `mif_pdata` reg-names "dit" |
| `EXYNOS_MODEM_SYSREG_BUSC_BASE+SIZE` | `0x1A220000` + `0x2000` | VERIFIED | `mif_pdata` reg-names "sysreg_busc" |
| `EXYNOS_MBOX_CP_BASE+SIZE` | `0x15920000` + `0x180` | VERIFIED | dtbo `mailbox_cp` reg |
| `EXYNOS_AP2CP_CFG_BASE+SIZE` | `0x15020000` + `0x40` | VERIFIED | `ap2cp_cfg_addr` prop; size = driver `ioremap(SZ_64)` `modem_ctrl_s5000ap.c:708` |
| `EXYNOS_PMU_CP_{CONFIGURATION,STATUS,CTRL_S,OUT,IN}` | `+0x3000/0x3004/0x3014/0x3020/0x3024` | VERIFIED | `pmucal_cp_exynos9820.h:2-25` (mirror); PMU base already VERIFIED |
| `EXYNOS_SMC_CP_PMU_ID` | `0x82000700` (READ_CTRL 3, WRITE_CTRL 4) | VERIFIED | `pmucal_cp.h:15-17` |
| `EXYNOS_CP_PKTPROC_GDESC_BASE` | `0xD0000000` | VERIFIED | `shm_ipc.c:535-543` (LINEAR_WA → spare base) |
| `EXYNOS_CP_ADDR_ALIAS_BASE` | `0x50000000` | **UNVERIFIED** | CP-view alias of AP `0xD0000000` — comment read (`shm_ipc.c:262-268`), closes at device observation (R2) |
| `EXYNOS_CP_RADIO_LBA_START/COUNT` | `57856` / `12800` (4096 B units) | VERIFIED | PIT entry 25, re-parsed in review |
| GSIVs: CP2AP_WAKEUP **407**, DIT_BUS_ERR **100**, DIT_RX_DONE **99**, DIT_TX_DONE **98**, MBOX_CP **73** | SPI+32 | VERIFIED (SPI cells) + repo rule | `mif_pdata`/`mailbox_cp` `interrupts`; cp_wdt==CP2AP_WAKEUP line `modem_ctrl_s5000ap.c:683-691` |

Memory map (`PlatformMemoryMapLib.c`): five new Device entries — `MODEM_DIT`,
`MODEM_BUSC`, `MBOX_CP`, `AP2CP_CFG`, `CP_IPC_SHMEM` (the last is CP-owned DRAM
above the published bank0 stop; mapped Device so a UEFI PoC touches the SBD
rings uncached like the vendor driver's ioremap).

**Build evidence (Linux verification, gcc 12.3.1, recipe regenerated from
`notes/d1-build-env.md` into `/tmp/opencode/build-uefi-linux.sh`)**:
`build.py -a AARCH64 -t GCC -p ../Platform/Beyond0LtePkg/Beyond0LtePkg.dsc -b DEBUG -j 24`
exit 0; `BEYOND0LTE_UEFI.fd` 7 340 032 B sha256 `91068362…`; `FVMAIN_COMPACT`
421 888 B total / **736 B free**; `FVMAIN` 47% full. No new diagnostics (the
`Error 1 (ignored)` line is the known make-recipe quirk, present in the prior
green build too). `strings PlatformMemoryMapLib.obj` carries all five new names.
**Verification-only FD — never flash it.**

### 2026-10-12 — T13: firmware-half first slice — `Beyond0LteModemDxe` (R1 pre-read + R2 observation)

- New DXE module `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteModemDxe/`
  (FILE_GUID `…554A`): reads PMU `CP_CONFIGURATION/CP_STATUS/CP_IN` **before
  writing anything** (closes risk P2 — did sboot pre-boot the CP?), prints
  `[CP] R1 pre: …`, then dumps mailbox `ISSR0..3` + `INTSR0`
  (`[MBOX] R2: …`) and the decoded `sbi_cp_status` field.
- **DECISION recorded in-source**: the `AP2CP_CFG=1` write (Android's own
  `s5000ap_on` first step) is compiled out behind `BEYOND0LTE_MODEM_INIT 0` —
  the first Windows boots stay free of new side-effect classes; flipping the
  macro to 1 IS rung R1 proper, and the task for it is the boot-stream task
  (same gate as every other D9 rung).
- **Build evidence** (Linux verification, gcc 12.3.1, same regenerated script):
  exit 0; `BEYOND0LTE_UEFI.fd` 7 340 032 B sha256 `cbb55d46…`;
  `FVMAIN_COMPACT` 421 888 B / **32 B free** (auto-sized FV — always ~full, the
  FD has MBs of slack, cf. D1 row); `FVMAIN` 47%. FFS
  `11111111-2222-3333-4444-55555555554ABeyond0LteModemDxe` present; the literal
  `[CP] R1 pre: CONFIGURATION=%x STATUS=%x IN=%x` found in `FVMAIN.Fv` strings.
  **Verification-only FD — never flash.**
- Not booted, not validated: like every D4b/D4c module, the device rungs ride
  the boot stream (M8/M9).

### 2026-10-13 — T14: Policy boundary pinned — RE of closed vendor binaries runs in a gitignored workspace (commit ban unchanged)

- The hard stop forbids **committing** reverse-engineered vendor internals, not
  the analysis itself. So the analysis runs in a gitignored workspace at the repo
  root: inspectable on disk, never tracked.
- Consequences: P5 gains a new option — static RE of `libsec-ril.so` / `cbd` in
  `re/` (gitignored). The commit ban, the blob ban and the scan gate are unchanged;
  committed files may carry **status pointers only** ("dialect characterized, see
  `re/FINDINGS.md` (not in tree)"), never the encoding itself.
- Workspace: `re/` at the repo root, added to `.gitignore`; tooling = xpack
  aarch64 binutils (`objdump`/`readelf`/`nm`) + python `capstone` in the already
  gitignored `.venv/`. Targets live in `dumps/` (already ignored) and the pulled
  copies under `re/`.

### 2026-10-13 — T15: RE pass #1 on the closed control plane (workspace `re/`, gitignored — details NOT committed)

- Executed design P5 option (e). Tooling: xpack aarch64 `readelf`/`nm -D`/
  `objdump`, strings, python byte-scan. Artifacts + findings live ONLY in `re/`
  (`FINDINGS.md`, `ipc_commands.txt`, pulled binaries) — per T14 the committed
  tree carries this status paragraph and nothing else.
- Verdict-level results (safe to state; internals stay in `re/`):
  - `libsec-ril.so` is **effectively unstripped** (10,253 dynamic symbols): the
    architecture is fully mapped — `IpcProtocol`/`IpcModem`/`ModemProxy` IPC
    layer + per-domain managers (Sim/Call/Sms/Network/DataCall/Epdg/…).
  - **Protobuf is narrow** (one embedded schema, carrier-name lookup only) — the
    CP dialect itself is hand-rolled, not protobuf.
  - **"MbimManager" is NOT MS-MBIM**: method names mirror MBIM commands, but the
    MBIM service GUIDs appear nowhere (byte-scan across `libsec-ril.so`,
    `libril_sem.so`, `vendor.samsung.hardware.radio.channel` HIDL lib → 0 hits).
    **The open-MBIM shortcut is dead** — the closed `IPC_*` command family is
    the only control path. (This closes a hope the second review had opened.)
  - The dialect self-describes partially in-binary (command-name tables);
    send-path trace and command-id extraction are queued in `re/FINDINGS.md`
    "Next RE steps".
- U10 status: **being characterized** — pass #1 done; layouts/ids deliberately
  not in the tree. Whether a Windows translator built from these findings ships
  is an open policy decision (design §8).

### 2026-10-13 — T16: CONTEXT CORRECTION — Windows boots; a separate agent builds the drivers

- Established for this stream: Windows already boots on the device, and a separate
  agent is building several drivers from this stream's research output. This
  stream's deliverable is research + PoCs + machine-readable task files that the
  driver agent consumes — not the boot-stream milestones themselves.
- Consequences for D9: the "device rungs ride on the boot stream" caveat is
  **stale for this stream** — R1/R2 observations are runnable NOW by the driver
  agent. This session does not touch the boot-stream milestone rows (M8/M9/T07 —
  that agent's territory, per repo rule on other agents' in-flight work); D9
  notes keep the fact instead. New task **R02** hands the driver agent the exact
  serial lines to read from the already-shipped `Beyond0LteModemDxe` slice.
- My deliverable class going forward: research + PoCs + machine-readable task
  files consumed by the driver agent.

### What is NOT yet known (each needs a source, not a guess)

- ~~U1~~ **CLOSED (T1, 2026-10-12)**: modem nodes = dtbo overlay `mif_pdata`
  (`sec_modem,modem_pdata`) + `mailbox_cp` (`samsung,exynos-shd-ipc-mailbox`) + `shmem`
  (`samsung,exynos-shm_ipc`) + rmem device nodes `modem_if`/`cp_ram_logging`/`modem_spare`.
  Residual CLOSED by T7: the 8 `gpios` phandles = `gpg3`/`gpg1` banks per the vendor dtsi (U7).
- ~~U2~~ **CLOSED — CORRECTED by T7 (mirror)**: the S10e's modem is the Exynos 9820's
  **integrated CP** (Samsung DT comment "Internal CP - S500AP", `exynos9820-rmem_s5100_dual.dtsi:40`;
  beyond0lte includes only `modem-s5000ap-sipc-pdata.dtsi`; `CONFIG_SEC_MODEM_S5100`/
  `CONFIG_LINK_DEVICE_PCIE` not set in the shipped Image; live `/reserved-memory` has no
  `modem_if2`/`s5100_msi`). The **Shannon S5100 is a discrete external companion chip for the
  S10 5G (beyondx) only** — the `s5100_*` pinctrl states / MSI window / pcie_1 doorbell in the
  S10e dump are family-wide leftovers. T5's "discrete S5100" reading is superseded.
- ~~U3~~ **CLOSED (T6+T7, mirror source)**: SBD = MIPI LLI IPC; g_desc/rb_desc/rbps layouts with
  exact offsets+sizes; SIPC5 header 4/5/6 B + channel ids; doorbell = mailbox ISSR0+4*mbx,
  INTGR1; mailbox reg map 0x15920000; shmem IPC base 0xFB200000+0x500000 VERIFIED; pktproc ring
  at modem_spare 0xD0000000 (CP alias 0x50000000); DIT reg map @0x1A2A0000; CP boot via SMC
  0x82000700 + PMU 0x15860000 regs + AP2CP_CFG 0x15020000=1; boot state machine ordered.
  Residual (not blocking): DIT ring-descriptor field semantics beyond the reg map
  (`link_rx_dit.c` gdesc programming), `sysreg_busc` 0x1A220000 bit meanings, STD_UDL boot
  frame byte order — all readable from the same mirror if needed.
- ~~U4~~ **CLOSED from the dump side (T4)**: RIL = `/dev/umts_ipc0` (+`umts_ipc1` 2nd SIM,
  `umts_rfs0`, `umts_boot0`); misc chardev per io_device = `/dev/<iod,name>`; `cbd` owns boot
  channels; rmnet* are netdevs not chardevs. Residual: live `ls /dev` confirmation (R01) —
  expected names `umts_ipc0/1`, `umts_rfs0`, `umts_cass`, `umts_csd`, `umts_router`, `umts_dm0`,
  `umts_boot0`, `umts_ramdump0`, `multipdp*`, `ipc_loopback0`, `smd4`, `umts_ciq0`,
  `ramdump_memshare`, `mbin0`.
- ~~U5~~ **CLOSED (T4)**: CP firmware = `modem.bin` → **RADIO** partition (PIT vid 16,
  52 428 800 B), exposed as `/dev/mbin0`; `cm.bin` = TEE CryptoManager (NOT modem); NO modem blobs under
  /vendor/firmware (the `calliope_*` files are ABOX audio DSP — T4 correction); cpefs = ext4 on its own LUN mounted at
  `/mnt/vendor/cpefs`; boot daemon `/vendor/bin/cbd` (CP_BOOT via SMC for secure + dpram download).
- ~~U6~~ **CLOSED (T10)**: Windows 10/11 requires MBIM in the modem + (documented) USB
  presentation; non-USB = IHV lower-edge client driver translating MBIM (MBBCx/NetAdapterCx,
  NdisMediumWirelessWan raw-IP); inbox MBIM (wmbclass/MBCD) binds USB only; AT-only is not a
  Win10 product path; no HW-classification requirement — rmnet maps 1:1 onto MBIM CID pipes.
  "WFEM" is not a real MS term (dead-end queries in T10).
- U10 (new, from T6/T8/T11): the **SEC-RIL control encoding on umts_ipc0 is closed** — the only
  hard wall for cellular under Windows. **Status 2026-10-13: being characterized — RE pass #1
  done in the gitignored `re/` workspace (T15); internals deliberately not committed.**
  Closes by (cheapest first): live framed-packet capture on Android (mif pktlog device /
  `/dev/umts_dm0` DIAG is a separate closed dialect), TWRP dump of the IPC shmem at
  0xFB200000 (ring contents), or static RE of `libsec-ril.so` (in progress in `re/`).
  All device-side variants deferred (repo rule 8 / R01).
- U11 (new, from T8): whether the CP debug UART (behind `2cp_uart_sel`, `ttySAC2/3` in cbd)
  emits or accepts anything useful at runtime — closes by live console capture only; currently a
  documented dead end for the AT path.
- ~~U7~~ **CLOSED (T7, mirror)**: the 8 `mif_pdata` `gpios` = hw-rev sense bits —
  `&gpg3 4/3/2/1` = CP_REV0..3 (XGPIO27..24), `&gpg1 5/4/3/2` = HW_REV0..3 (XGPIO13..10)
  (`modem-s5000ap-sipc-pdata.dtsi:31-34` + comment); read by `get_system_rev()` into SBI
  `sbi_sys_rev`. Handshake GPIOs are NOT used by the s5000ap project (no pinctrl-0).
- U8 (new, from T4): is this unit ss or dsds (`ro.vendor.multisim.simslotcount`, set from
  `/mnt/vendor/efs/factory.prop`) — closes by: live `getprop` (R01). Affects whether
  `/dev/umts_ipc1` + `ril-daemon1` exist at runtime.
- U9 (new, from T4): cpefs contents (CP NV/calibration files) — names only — closes by: TWRP
  mount of `cpefs` (R01, read-only) — deliberately not touched now (repo rule 8).

## Review of the dump + mirror research passes (T1–T7, harsh-critic pass, 2026-10-12)

Checked mechanically (every one re-run/re-read on this box, not by re-reading the notes):

- **IKCONFIG claim (T3)**: `IKCFG_ST` found at file offset `0x109d4f0` in `dumps/hhxj6/kernel`;
  payload is gzip (raw zlib call fails, `wbits=16+15` works); config 174 792 B. Extracted config
  confirms verbatim: `CONFIG_SEC_MODEM_S5000AP=y`, `# CONFIG_SEC_MODEM_S5100 is not set`,
  `# CONFIG_LINK_DEVICE_PCIE is not set`, `# CONFIG_SEC_SIPC_DUAL_MODEM_IF is not set`,
  `CONFIG_CP_SECURE_BOOT=y`, `CONFIG_CP_LINEAR_WA=y`, `CONFIG_MCU_IPC=y`, `CONFIG_CP_DIT=y`.
  (This also **corrects the skill's evidence-ladder claim** "No embedded IKCONFIG" — for THIS
  Image the config IS recoverable. Recorded here because the next session will trust the ladder.)
- **Mirror citations (T6/T7)**: `cruel` sparse clone re-opened — `sbd_rb_desc` field order/widths
  match; `exynos9820-rmem_s5100_dual.dtsi` carries `/* External CP - S5100 */` over `modem_if2`
  and `/* Internal CP - S500AP */` over `modem_if`; `exynos9820-beyond0lte_common.dtsi:15` includes
  **only** `modem-s5000ap-sipc-pdata.dtsi`; that pdata dtsi has reg `<0 0x10000 0>, <0 0x1A2A0000
  0x10000>, <0 0x1A220000 0x2000>`, IRQs `<0 375 0>,<0 68 0>,<0 67 0>,<0 66 0>`,
  `ap2cp_cfg_addr=<0x15020000>`, `mif,link_attrs=<0x7C9>` with the bit-decode comment, `gpios =
  <&gpg3 4..1>, <&gpg1 5..2>`, mailbox `reg=<0 0x15920000 0x180>` SPI 41; **zero `pinctrl-0`** in
  the s5000ap pdata, and `s5100_evt1_dual_*` pinctrl is referenced only by
  `modem-s5100_evt1-dual-sipc-pdata.dtsi` — the T7 supersession of T5 holds.
  `pmucal_cp.h:15-17` = `SMC_ID 0x82000700`, READ_CTRL 3, WRITE_CTRL 4. `regs-mcu_ipc.h`:
  MCUCTLR@0, INTGR0@8, INTSR0@0x14, ISSR0@0x80.
- **Dump citations (T1/T4)**: `frag_dtb02.txt` — `mailbox_cp` reg/SPI, `shmem` partition cells,
  `umts_ipc0 iod,id=0xeb`, `iod,ul_num_buffers=<0x10>` (confirms T7's correction of T1's
  "10×" hex misread); dtb00 root compatible `samsung,BEYOND0LTE EUR OPEN 17`. PIT re-parsed with
  the `notes/pit-geometry.md` layout: RADIO=`modem.bin` vid **16** start 57856 len 12800;
  CPEFS vid 1 start 6 len 1536; CM start 0 len 1792; CP_DEBUG 2149888+1280.
  `init.baseband.rc` via debugfs: `/dev/mbin0` symlink, `cbd -tss310` service, cpefs mount — verbatim.
- **Repo cross-cites**: `notes/m12-wifi-chip.md:88`, `notes/dock-desktop.md:545`, and
  `notes/s10e-live-reads.md:125-127` (live rmem children: `modem_if`, `modem_spare`,
  `cp_ram_logging` present; **no** `modem_if2`/`cp2cp_shm`/`s5100_msi`) — all exact.
- **Windows contract (T10)**: the cellular-architecture page re-fetched — the two load-bearing
  sentences ("Implement the **MBIM protocol interface in your modem hardware**… or **another
  interface that presents itself as a USB host controller**"; "For **PCIe** cellular modem
  devices, the IHV vendors are expected to provide an **IHV client driver that translates the
  MBIM commands**…") are verbatim.

Found:
1. **Title/plan-of-record names the wrong chip**: notes title, D9 milestone row, progress.md
   heading and the task title all say "Shannon S5100 modem" — disproved for this board by T7
   (S5100 = beyondx/5G companion; S10e = integrated CP, project S500AP/S5000AP).
2. T1's buffer-count misread and T2's 4+4 GPIO inference — both already corrected **in place**
   by T7; no further action.

Fixed: rename commit follows this one. T0 heading annotated as superseded on U2.
Not affected: all constants/values themselves — every sampled number matched its source exactly.

## Review of the constants pass (T12, harsh-critic pass, 2026-10-12)

- Checked: PMU CP register offsets + bits re-read from `pmucal_cp_exynos9820.h:2-25` verbatim
  (CTRL_S 0x3014 bit3, CONFIGURATION 0x3000 bit0, STATUS 0x3004 bit0, IN 0x3024 bit4,
  OUT 0x3020 bit16, all on 0x15860000); `shm_ipc.c:535-543` really does pick
  `pdata.p_spare_addr` under `CONFIG_CP_LINEAR_WA`; `modem_ctrl_s5000ap.c:683-691` really
  requests `platform_get_irq(pdev,0)` as `cp_wdt` (= DT IRQ 0 = CP2AP_WAKEUP SPI 375);
  GSIV arithmetic 375/68/67/66/41 + 32 re-computed; pairwise overlap check of the five new
  windows against each other and against CMU_BANK0/PMU/ALIVE_GPIO — none;
  `strings PlatformMemoryMapLib.obj` shows all five names; FD numbers re-read from the log.
- Found: **no defect in this commit.** Adjacent pre-existing observation (NOT touched):
  `PlatformMemoryMapLib.c:61-62` claims "CMU_TOP (0x1A240000) sits INSIDE CMU_BANK0
  (0x1A200000 + 0x8000)" — arithmetically false (0x1A240000 is 0x40000 above the bank end),
  and `CMU_TOP` is not in the MMIO table, so it is unmapped today. Nothing in our firmware
  touches CMU_TOP, so this is a comment defect + an open mapping question for whoever first
  needs CMU_TOP; recorded here, deliberately not fixed in a D9 commit.
- Fixed: n/a (nothing in the diff needed a follow-up).
- Not affected: the constants themselves — every value matched its cited line.

## Review of the design doc (T11, `D9-MODEM-DESIGN.md`, harsh-critic pass, 2026-10-12)

- Checked: `sipc5.h:261-297` really carries the STD_UDL boot framing the ladder leans on
  (`STD_UDL_SEND 0x1`, `STD_UDL_CRC 0xC`, `struct std_dload_info {size,mtu,num_frames}`);
  `sbd.h` signaling comment really says `0 (polling), 1 (interrupt)` (R5's plain-ring claim);
  modem.bin TOC @0xa0 is cited in notes T8 (`shm_ipc.c:865`); `SAMS9827` free — the built
  `Dsdt.aml` carries only SAMS9820/9821/9825/9826; MBBCx OID→MBIM sentence verbatim from the
  re-fetched architecture page.
- Found: (1) P7 stated the cpefs→RFS→CP link as cited fact — it is inference; reworded to
  say so. (2) "no mainline driver exists" was an uncited negative — softened to
  "no mainline driver is known" + the exact zero-hit query; elixir.bootlin.com is
  bot-walled (dead end, do not retry that URL).
- Fixed: both, in review-fix passes inside this arc.
- Not affected: the ladder order, the two-halves split, the constant table (all re-verified
  at the T12 constants review).

## Review of the ModemDxe module (T13, harsh-critic pass, 2026-10-12)

- Checked: `regs-mcu_ipc.h:18-32` re-read — INTSR0 really is `0x14`, ISSR0..3 really
  `0x80/84/88/8c` (the module's `ISSR0 + Try*4` stride matches); `sbi_cp_status`
  mask/pos 0xf@1 matches T1's DT read and T7's `boot_on` poll semantics (ISSR3 =
  cp-side united status); PMU offsets come from the VERIFIED header macros, not
  restated; Depex mirrors the PCIe module (IoLib + CPU arch protocol); the reads
  touch only windows the constants commit mapped (PMU, MBOX_CP) — the AP2CP_CFG
  path is compiled out, so the module performs **zero writes** at DXE entry, which
  keeps the first boot free of new side-effect classes as claimed; FFS GUID + the
  literal debug string verified in the built `FVMAIN.Fv`; "32 B free" is the known
  auto-sized-FV behaviour (D1 row), not a space hazard.
- Found: no defect.
- Fixed: n/a.
- Not affected: the staged R3–R5 work (not written yet — no claim made).

## Second-opinion review of the whole D9 arc (2026-10-12, adversarial pass #2)

Scope: every claim the per-commit reviews had NOT re-opened, plus checklist Q4
(uncompiled guard branch) and the boot-hazard class (clock state of blocks the PoC
reads at DXE entry). All checks mechanical; scratch clones/dumps under
`/tmp/opencode/modem-wwan/` still present and re-used.

Checked and held (values AND citations):
- `sbd.h:60-73` region map (CMD_DESC 64K: CTRL 1K/CMD 1K/DESC 62K, BUFF @64K);
  `sbd_global_desc` field order (`sbd.h:143-168`); `MAX_SIPC_CHANNELS 256` /
  `MAX_LINK_CHANNELS 32` (`modem_v1.h:80-81`); `sbd.h:18-24` "MIPI Alliance".
- RBP arrays `[ul_rp][ul_wp][dl_rp][dl_wp]` (`link_device_memory_sbd.c:255-258`)
  and `rb->rp = &sl->rp[dir][id]` (:182-183).
- Doorbell chain: `send_ap2cp_irq` = `mbox_set_value(MCU_CP, mbx_ap2cp_msg, mask)`
  + `mbox_set_interrupt` (`link_device_shmem.c:2903-2907`); slots `ISSR0+4*n`,
  n<64 (`mcu_ipc.c:257-271`); `INTGR1`, `1<<int_num`, int_num<16 (:238-244);
  CP→AP status `INTSR0 & 0xFFFF0000`, clear via `INTCR0` (:59/:67); compatible
  table incl. `samsung,exynos-shd-ipc-mailbox` (:529-539); `mbox_sw_reset` (:301).
- SBI-in-mailbox claim: `mbox_update_value(MCU_CP, ap_status, …, sbi_crash_type_*)`
  at `link_device_shmem.c:401` ✓.
- SIPC5 header (`sipc5.h:22-36`: cfg/ch/len/union, START_MASK 0b11111000,
  PADDING bit2/EXT bit1/CTL bit0); channel ids `SIPC_CH_ID_PDP_0=10`, `CASS=35`,
  `BOOT_0=215`, `DUMP_0=225`, `FMT_0=235`, `RFS_0=245` (`modem_v1.h:139-199`);
  `LINKTYPE()` = `1u<<pos` with `LINKDEV_SHMEM`=9 → `0x200` ✓ and
  `LINKDEV_PCIE`=11 → `0x800` ✓ (ties T1's `mif,link_types=<0x200>` and T7's
  s5100 `0x800` comment together — both right).
- `misc_write` copies verbatim (`modem_io_device.c:1063`); `miscdev.name =
  iod->name` + `misc_register` (:1551-1555); `mld->base = shm_get_ipc_region()`
  (:3964) → `pdata.p_addr + pdata.ipc_off` (`shm_ipc.c:509`).
- s5000ap anchors: `get_system_rev` :117, `s5000ap_on` :302, AP2CP_CFG write-1/
  readback-1 :333-342 (verbatim match to the module comment), `init_mailbox_regs`
  :203, `s5000ap_reset` :404 with `0xDEB` check at **:418**, `boot_on` :445,
  `boot_off` :479; `pmucal_cp.c` SMC calls :229-298; `dit_init_hw` :435 with
  0x4020/0x1/0x7; `dma_zalloc_coherent` :732; TOC "0xa0: cp memory map offset"
  `shm_ipc.c:865-866`.
- Dump side: strings cites exact (`kernel.strings.txt:48663-48668`,
  217627-217629, 124364 `eur_open`, 48671 `link_device_shmem.gro_flush_time`);
  4 src/ stubs are 14 B; `soc.Makefile:57-60` pmu-cp.o block; all 7 dtbo root
  compatibles = BEYOND0LTE EUR OPEN 17/18/19/20/22/24/25; modem fragments
  identical across all 7 except `phandle`/`fragment@` lines (diff proved);
  PIT dtype=8 for all 41 entries, CM vid 91; `cbd` 436 432 B, `libsec-ril.so`
  4 077 344 B; `s10e-partitions.md:9-10` bota0=sda1 / cpefs=sdd1.
- Windows pages re-fetched verbatim: mb-miniport-driver-types (production row:
  `NdisMediumWirelessWan`/EnableDhcp 0/ARP not required; INF `IF_TYPE_WWANPP/2`,
  LowerRange "ppip"); mb-interface-model (MBCD inbox since Win8, compat ID
  "MBIM"); mbnapi (1803 replaced-by-WinRT notice).
- **Q4 — the never-compiled branch**: flipped `BEYOND0LTE_MODEM_INIT` to 1,
  full Linux build exit 0 (module recompiled, FVMAIN_COMPACT 421 888 B / 0 free
  — it fits), flipped back, rebuild exit 0 (24 B free; FV fill varies across
  rebuilds — compare sizes not hashes, per repo rule). The guard compiles both
  ways.
- **Boot hazard — clock state at DXE entry**: `mcu_ipc.c` contains **no clock,
  PMU or power-domain handling at all** (grep clk/pmu/speedy/power → nothing)
  and the `mailbox_cp` DT node has no `clocks`/`power-domain` property — the
  vendor kernel probes and reads the block raw, so the PoC's reads of
  `0x15920000` are in the same safety class as the vendor's own probe. PMU
  (`0x15860000`) is the system controller, already read by other flows.
  No gating hazard found in the first slice.

Found (all fixed in follow-up commits):
1. **T9's verdict was over-generalised** — "no device shown has working cellular
   under Windows" is FALSE: Mu Status.md carries `Mobile Data ✅` in **Windows**
   status tables for at least 5 Snapdragon phones (cepheus :3325, vayu :3391,
   lisa :4094 "SIM2 doesn't work", surya :4248, miatoll :4379 — each under "Needs Windows
   Drivers"; nuwa/starqltechn/miatoll's other ✅ are Linux tables. vayu was
   unclassified in the first cut of this fix and added during the review of this
   pass's own commits — self-caught, 4→5). The pass read
   only to ~line 1511; the first ✅ is at 1588. The Exynos/Shannon-specific claim
   stands (all ✅ devices are Qualcomm modems with vendor IHV driver releases) —
   and the corrected fact SUPPORTS the design: the lower-edge IHV shape works on
   phones under Windows; what Exynos lacks is the translator, not the architecture.
2. T6's ack-mask list "REQ_ACK/RES_ACK FMT/RAW 0x20/0x08/0x10/0x04" mis-pairs:
   actual `REQ_ACK_FMT/RAW = 0x20/0x10`, `RES_ACK_FMT/RAW = 0x08/0x04`
   (`link_device_memory.h:153-156`).
3. Vendor AP always writes `signaling = 1` (`setup_sbd_rb_desc`,
   `link_device_memory_sbd.c:110`) — the polling path (signaling=0) is OUR
   construction, never exercised by the vendor; design P4 must say so.
4. Citation offsets: `0xDEB` check is `modem_ctrl_s5000ap.c:418` (notes said
   :414); ipc physical add is `shm_ipc.c:509` (notes cited the 510-515 range).
5. T4's "/firmware 69 files + 2 dirs" — actual: **67 regular files + 2 subdirs**
   (nfc, score) = 69 entries.
6. Vendor forces no IRQ trigger type (`IRQF_NO_SUSPEND|IRQF_NO_THREAD` only,
   `modem_ctrl_s5000ap.c:671`; mcu_ipc requests with flags 0, :472) — the DSDT
   `Interrupt()` polarity must come from the live `/proc/interrupts` type column;
   R01 now captures it.
7. `progress.md` WWAN block didn't list the module slice as done.

Not affected: every constant in `Exynos9820.h` (re-verified at the T12 constants review and
again here via the doorbell/ids batch), the memory map, the ladder order, the
U10 closed-wall verdict, the two-halves split.

## Review of the RE policy + pass-1 status entries (T14 + T15, harsh-critic pass, 2026-10-13)

- Checked: `git check-ignore -v` proves `re/FINDINGS.md`, `re/libsec-ril.so`,
  `re/ipc_commands.txt` are all ignored by `.gitignore:62`; `git status` clean
  with `re/` populated; the two committed diffs grepped for leakage
  (`0x…{4,}|struct|layout|offset`) — the only hits are a policy sentence and the
  pre-existing DT constant `0xFB200000`; class names in the status paragraph are
  exported dynamic symbols (visible to any `nm -D`), not RE'd internals; the
  MBIM-GUID negative is a verdict, not an encoding.
- Found: no leakage. One scope note: the second-opinion review's "the IHV shape
  is proven" line stays true (Snapdragon), but pass #1 CLOSED the specific hope
  that Samsung's "Mbim" naming implied an open MBIM path — design P5(a) "does
  the CP expose any open control surface" now has a static-RE answer: not via
  MBIM; the R5 live probe remains the dynamic check.
- Fixed: n/a (P5(a) wording left as the live-probe question it always was).
- Not affected: the ladder, the constants, the boot/link contracts (all open-source
  side, untouched by this RE pass).

## Arc close-out: collapse to one commit + attribution scrub (2026-10-13)

- The arc's 25 per-task commits were collapsed into one once the arc finished
  (`research: D9 WWAN arc (squashed): …`). The pre-collapse tip is kept as the tag
  `d9-pre-squash` while the collapse is checked.
- Scrub: committed markdown no longer quotes or attributes decisions to a named
  person. Reworded: T14 (policy boundary, quote removed), T16 (context correction,
  quote removed), design P5 + §8, HANDOFF §26 item 7 and §28, the `progress.md` /
  `tasks/R02` boot-status lines, the `.gitignore` comment. Facts, dates and the `re/`
  rule (analysis in, nothing committed) are unchanged.
- Dead references fixed: the review headings above named per-task commit hashes,
  which the collapse invalidated; they now name the task (T1–T7, T11, T12, T13,
  T14+T15). `git grep` over the tree finds no dead hash.
- Defect found and fixed here: the D9 row in `DRIVERS-MILESTONES.md` had swallowed
  the D8 row's notes cell (D8 left with 3 cells, D9 with 4). Both rows are back to
  4 cells.
- Verified mechanically after the collapse: `git diff --stat d9-pre-squash HEAD`
  shows only the 7 scrubbed markdown/config files; every code file is byte-identical
  (sha256 over `Beyond0LteModemDxe.c/.inf`, `Exynos9820.h`, `PlatformMemoryMapLib.c`,
  `.dsc`, `.fdf`); `re/` and `dumps/` still ignored (`git check-ignore` →
  `.gitignore:62`/`:58`, the line the review above cites is still correct); the 9810
  tree untouched; `tools/release/scan.py` = 86 findings before and after, zero in a
  D9 file. No code changed, so the arc's Linux verification builds still stand; no
  device rung has run, so D9 stays unvalidated.

## Sources

- `dumps/hhxj6/base.dts`, `dumps/hhxj6/kernel` (strings), `dumps/hhxj6/dts/` — shipped Android 12
  dump `G970FXXUHHXJ6`.
- `notes/m12-wifi-chip.md` (pcie_1 = modem doorbell, disabled), `notes/dock-desktop.md` §17,
  `notes/vendor-dt-reserved.md`, `Exynos9820.h:75-80,155-164` — repo facts, cited above.
- 2026-10-12 LOCAL-DUMP pass artefacts (scratch, `/tmp/opencode/modem-wwan/`): `walk_dt.py`,
  `dump_frag.py` (raw-DTB walkers over `tools/dump-tools/fdt.py`), `frag_dtb0*.txt` (full modem
  fragments per SKU), `kernel.strings.txt` (`strings -a kernel`), `kernel.config` (IKCONFIG gzip at
  Image file offset `0x109d4f0`), `libsec-ril.so`/`cbd` (debugfs dumps, strings-only use).
- `dumps/hhxj6/BEYOND0LTE_EUR_OPEN.pit` (struct parse), `dumps/hhxj6/vendor/vendor.raw`
  (`debugfs -R 'ls -l|cat|dump'` on /firmware, /lib64, /bin, /etc/init/*.rc, /build.prop — names
  and sizes only, no blob contents recorded).
- `notes/pit-geometry.md` (PIT decode method), `notes/cpu-driver-9820.md:100,228-229` (cm.bin=TEE,
  BL tar contents), `tools/dump-tools/kallsyms.py` header (no-kallsyms negative result).

### Sources added by the mirror/prior-art/Windows pass (2026-10-12)

- Mirrors (branches verified via `git ls-remote`, sparse clones in `/tmp/opencode/modem-wwan/`):
  - `https://github.com/CruelKernel/samsung-exynos9820` branch `cruel-HVJ5-v5.3` (HEAD 41aba776)
    — `drivers/misc/modem_v1/*`, `drivers/misc/mcu_ipc/*`, `drivers/soc/samsung/cal-if/*`,
    `arch/arm64/boot/dts/exynos/modem-s5000ap-{,dual-}sipc-pdata.dtsi`,
    `arch/arm64/boot/dts/exynos/exynos9820-rmem_s5100_dual.dtsi` / `_s5000ap.dtsi`,
    `arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi` (+ eur_open_17..25.dts).
  - `https://github.com/LineageOS/android_kernel_samsung_exynos9820` branch `lineage-19.1`
    (cross-check clone).
  - `https://github.com/GoRhanHee/android_kernel_samsung_exynos9820` (branches exist; not used).
- `https://github.com/Project-Silicium/Mu-Silicium` — `Status.md` (raw) + `S5E9820Pkg` tree
  (`git ls-tree`); old `Mu-Silicium/edk2-platforms` URL now redirects to auth prompt (dead end).
- `https://github.com/sonic011gamer/edk2-exynos` (master c0003541) — tree grep, zero modem code.
- `https://github.com/ntdevlabs/exynos9810-woa` README (raw) — driver set has no WWAN.
- Samsung silicon statements:
  `https://semiconductor.samsung.com/news-events/tech-blog/exynos-9820-delivering-groundbreaking-features-on-the-galaxy-s10/`
  ("the integrated modem supports LTE Category 20"),
  `https://news.samsung.com/global/how-the-exynos-9-series-9820-is-ushering-in-a-new-era-of-mobile-experiences`
  ("The Exynos 9820 integrates an LTE-Advanced Pro modem").
- Windows contract (all fetched 2026-10-12):
  `https://learn.microsoft.com/en-us/windows-hardware/drivers/network/cellular-architecture-and-driver-model`,
  `.../network/mb-interface-overview`, `.../network/mb-miniport-driver-types`,
  `.../network/mb-interface-model`, `.../network/mb-raw-ip-packet-processing-support`,
  `.../netcx/mobile-broadband-mbb-wdf-class-extension-mbbcx`,
  `https://learn.microsoft.com/en-us/windows/win32/api/mbnapi/nn-mbnapi-imbnconnection`,
  `https://learn.microsoft.com/en-us/windows/win32/mbn/mobile-broadband-networks-portal`,
  `https://learn.microsoft.com/en-us/uwp/api/windows.networking.networkoperators.mobilebroadbandmodem`.
- Dead-end queries recorded in place (T8 AT greps, T9 web searches, T10 WFEM/wwanapi/AT-deprecation
  searches) — do not repeat verbatim.
- 2026-10-13 RE pass (T15): `re/libsec-ril.so`, `re/libril_sem.so`, `re/cbd`,
  `re/vendor.samsung.hardware.radio.channel` HIDL .so (debugfs dumps, gitignored),
  `re/FINDINGS.md` + `re/ipc_commands.txt` (analysis, gitignored). Tooling:
  xpack aarch64 binutils + python byte-scan. Verdict-level facts only are
  committed; the artifacts and internals stay in `re/` per the T14 policy boundary.
