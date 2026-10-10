/* @file
 * bootshim.S — relocating loader that prepends the EDK II firmware device (FD).
 *
 * Adapted from tools/BootShim/BootShim.S of edk2-msm (Renegade Project,
 * https://github.com/edk2-porting/edk2-msm), as carried by edk2-exynos
 * (https://github.com/sonic011gamer/edk2-exynos): the fake Linux kernel header
 * and the relocating copy below are theirs. Copyright (c) Renegade Project and
 * the edk2-msm authors; BSD-2-Clause, full notice in this repository's NOTICE.
 *
 * The Galaxy S10e bootloader (sboot) loads an Android boot.img "kernel" and
 * jumps to its first byte. Our EDK II FD (BEYOND0LTE_UEFI.fd) is *linked to run*
 * at 0x80080000 (Beyond0LtePkg.dsc:415), but where sboot actually drops the
 * kernel is NOT a read on this SoC: the comment in the 9810 shim computes
 * DRAM base (Exynos9820.h:79) + text_offset (0x80000) = 0x80080000, while that
 * same comment says 0x80100000 - the two disagree by one 0x80000 and nothing
 * on the 9820 closes it (K4b, UNVERIFIED). This shim bridges the gap for
 * whichever address sboot picks, subject to the self-overwrite hazard noted at
 * the copy step.
 *
 * Layout after packaging:  [ this shim ][ BEYOND0LTE_UEFI.fd ]   (cat'd together)
 *
 * Flow (inherited from the proven edk2-exynos tools/BootShim + m1-hello start.S,
 * device-verified on star2lte ONLY - no stage of this flow has been validated on
 * an S10e, see MILESTONES):
 *   1. sboot jumps to offset 0 -> `adr x1,_payload` captures the *runtime*
 *      address of the appended FD, then branch to _start.
 *   2. mask IRQs, stop the SoC watchdog, disable D/I caches (MMU is OFF at
 *      handoff, so these are direct device/RAM stores).
 *   3. copy the FD (UEFI_SIZE bytes) to UEFI_BASE (0x80080000). On the 9810 this
 *      was clean RAM ABOVE the RKP-monitored kernel zones (0x80094000-0x828DBFFF);
 *      on the 9820 there is NO read of the equivalent write-protected zones (K10,
 *      UNVERIFIED), and 0x80080000 is itself only a decision, not an observation.
 *   4. branch to UEFI_BASE. The FD's first word is an ARM64 `B` that GenFv wrote
 *      into the FV-header zero-vector, jumping to the PeilessSec SEC entry
 *      (checked mechanically on BEYOND0LTE_UEFI.fd: first word 0x1401C800).
 *
 * The shim is fully position-independent (PC-relative adr / literal ldr /
 * register br), so it runs correctly wherever sboot places it.
 *
 * Build: clang -target aarch64-none-elf -c bootshim.S -o bootshim.elf
 *        llvm-objcopy -O binary bootshim.elf bootshim.bin
 * (No link step needed — there are no absolute relocations.)
 */

#ifndef UEFI_BASE
#define UEFI_BASE 0x0000000080080000   /* must equal PcdFdBaseAddress (Beyond0LtePkg.dsc:415 / .fdf:69). UNVERIFIED as an OBSERVED drop address: the initrd bound is the read, this base is the decision that follows. */
#endif
#ifndef UEFI_SIZE
#define UEFI_SIZE 0x0000000000700000   /* VERIFIED: built BEYOND0LTE_UEFI.fd is exactly 7 340 032 B = 0x700000 */
#endif

/* ---- DIAG breadcrumb channel ------------------------------------------------
 * The 9810's ramoops base 0xFED14000 is INSIDE debug_snapshot's log regions on
 * this SoC (0xFD900000..0x100000000, Exynos9820.h:112-115) - writing there is a
 * verified wrong. The gap 0xD9801000..0xF0000000 (between s5100_msi, Exynos9820.h:102-103,
 * and modem_if, :104-105) is unclaimed by any read DT node, so it is the
 * candidate - but it is a DECISION, not a read, and enabling it is unvalidated.
 * Default 0 mirrors BEYOND0LTE_PRAM_DIAG 0 in ArmPlatformLib.c:31,
 * Beyond0LteResetSystemLib.c:59, PlatformMemoryMapLib.c:48,
 * Beyond0LteAcpiPlatformDxe.c:57. */
#ifndef BEYOND0LTE_PRAM_DIAG
#define BEYOND0LTE_PRAM_DIAG 0
#endif
#ifndef BEYOND0LTE_PRAM_BASE
#define BEYOND0LTE_PRAM_BASE 0x00000000D9801000   /* UNVERIFIED (gap choice, see above) */
#endif

	.text
	.global _head
_head:
	adr	x1, _payload            /* x1 = runtime address of the appended FD   */
	b	_start                  /* (this pair is the header's code0/code1)   */
	/* ---- Linux ARM64 Image header (values read for SM-G970F) ----------- */
	.quad	0x0000000000080000      /* text_offset: VERIFIED - the SM-G970F kernel Image artefact (C:\Users\ilya\code\Dumps\twrp_device_tree\kernel, notes §10) declares 0x80000 */
	.quad	0x0000000000800000      /* image_size: UNVERIFIED requirement - this is a shim+FD+margin guess; the vendor kernel declares 0x03138000 for itself */
	.quad	0x000000000000000a      /* flags: VERIFIED 0xa in that same artefact (4K pages, load anywhere, LE) */
	.quad	0                       /* res2                                      */
	.quad	0                       /* res3                                      */
	.quad	0                       /* res4                                      */
	.ascii	"ARM\x64"               /* magic = 0x644d5241                        */
	.long	0                       /* res5 (PE/COFF header offset; none)        */

_uefi_base:
	.quad	UEFI_BASE
_uefi_size:
	.quad	UEFI_SIZE
#if BEYOND0LTE_PRAM_DIAG
_pram_base:
	.quad	BEYOND0LTE_PRAM_BASE
#endif

_start:
	msr	DAIFSet, #0xf           /* mask D/A/I/F (all interrupts)             */

	/* ---- Stop the SoC watchdog FIRST ----------------------------------
	 * sboot arms watchdog_cl0@0x10050000 and watchdog_cl2@0x10060000 - the
	 * SECOND counter on this SoC is cl2, not the 9810's cl1 (Exynos9820.h:426,
	 * :431 EXYNOS_WDT_BASE: both BASES VERIFIED on the S10e). WTCON=0 halts
	 * the counters before they can fire. The register OFFSET (window+0x00) is
	 * UNVERIFIED: EXYNOS_WDT_SIZE:432 is an unread extent. MMU is OFF at handoff
	 * on the 9810 (device-verified there, UNVERIFIED here) -> direct device stores.
	 * x0 is scratch (x1 holds the payload pointer and is preserved). */
	movz	x0, #0x1005, lsl #16    /* 0x10050000 (cl0 WTCON, offset 0x00)       */
	str	wzr, [x0]
	movz	x0, #0x1006, lsl #16    /* 0x10060000 (cl2 WTCON)                    */
	str	wzr, [x0]
	dsb	sy

	/* ---- Disable D/I caches before copying ----------------------------
	 * MMU is OFF at handoff (device-verified on star2lte; UNVERIFIED on the 9820,
	 * K9 in notes/m8-uefi-image.md), so data stores reach RAM.
	 * Clearing I prevents a stale instruction-cache fetch of the freshly
	 * copied FD. EDK II re-enables MMU + caches properly in CpuDxe. */
	mrs	x0, SCTLR_EL1
	bic	x0, x0, #(1 << 2)       /* C = 0 : data cache off                    */
	bic	x0, x0, #(1 << 12)      /* I = 0 : instruction cache off             */
	msr	SCTLR_EL1, x0
	isb

#if BEYOND0LTE_PRAM_DIAG
	/* ---- DIAG breadcrumb 'A': shim entry, via persistent RAM ----------
	 * The 9810's 0xFED14000 is VERIFIED WRONG here: debug_snapshot's log regions
	 * occupy 0xFD900000..0x100000000 on this SoC (Exynos9820.h:112-115), so that
	 * write lands in a vendor log buffer. The base used is BEYOND0LTE_PRAM_BASE,
	 * the unclaimed gap 0xD9801000..0xF0000000 (Exynos9820.h:102-105) - a DECISION,
	 * not a read, and this whole channel is compiled out (BEYOND0LTE_PRAM_DIAG 0)
	 * until it is chosen deliberately. m1-format ramoops pmsg header (sig 'DBGC',
	 * start=0) then text 'A' with size=1; TWRP exposes it at
	 * /sys/fs/pstore/pmsg-ramoops-0. MMU off -> direct DRAM stores, dsb is enough.
	 * x12/w13 scratch; x1 kept. */
	ldr	x12, _pram_base           /* x12 = BEYOND0LTE_PRAM_BASE              */
	movz	w13, #0x4244
	movk	w13, #0x4347, lsl #16   /* w13 = 0x43474244 = 'DBGC' ramoops sig     */
	str	w13, [x12]              /* [0] = sig                                 */
	str	wzr, [x12, #4]          /* [4] = start = 0                           */
	mov	w13, #0x41              /* 'A'                                       */
	strb	w13, [x12, #12]         /* text[0] = 'A'                             */
	mov	w13, #1
	str	w13, [x12, #8]          /* [8] = size = 1                            */
	dsb	sy
#endif

	/* ---- DIAG: paint RED band (rows 200-649) — shim entry reached ------
	 * Direct stores to the panel scanout buffer. The 9810's 0xCC000000 at
	 * 1440x2960 is WRONG here: this phone's framebuffer is EXYNOS_FB_BASE
	 * 0xCA000000 at 1440x3040 (Exynos9820.h:450,:452, read from the live
	 * cmdline s3cfb.bootloaderfb=0xca000000). Stride is unchanged (width
	 * 1440), so the row offsets keep their low halves: row 200 =
	 * 0xCA000000 + 200*1440*4 = 0xCA119400. MMU off = physical address.
	 * Proves sboot jumped to us and stores work. x1 (FD pointer) preserved
	 * (we use x9-x11). */
	movz	x9, #0x9400
	movk	x9, #0xCA11, lsl #16    /* 0xCA119400 = FB + row 200                 */
	movz	w10, #0xE340
	movk	w10, #0x9, lsl #16      /* 648000 px = 450 rows x 1440                */
	movz	w11, #0x0000
	movk	w11, #0xFFFF, lsl #16   /* 0xFFFF0000 = opaque RED (BGRA)            */
.Lred:
	str	w11, [x9], #4
	subs	w10, w10, #1
	b.ne	.Lred
	dsb	sy

	/* ---- Copy the appended FD to UEFI_BASE ----------------------------
	 * dst (x5) = UEFI_BASE, src (x4) = just past this shim. On the 9810 the two
	 * were 0x10000000 apart. HERE THEY MAY NOT BE: if sboot drops the kernel at
	 * 0x80080000 or 0x80100000 (K4b, UNVERIFIED, not closable by a cheap read),
	 * the dst range 0x80080000..0x80780000 covers the running shim and the copy
	 * eventually fetches overwritten instructions. The live initrd bound
	 * (0x89000000) would be clear of it; the vendor DT bound (0x84000000) would
	 * not. Ascending order stays safe against the source itself (dst < src). */
	ldr	x5, _uefi_base          /* x5 = UEFI_BASE (destination)              */
	mov	x7, x5                  /* x7 = destination base, kept for the jump  */
	mov	x4, x1                  /* x4 = source (FD runtime address)          */
	cmp	x4, x5
	b.eq	2f                      /* already at the link address               */
	ldr	x6, _uefi_size          /* x6 = byte count                           */
1:	ldp	x2, x3, [x4], #16
	stp	x2, x3, [x5], #16
	subs	x6, x6, #16
	b.gt	1b
	mov	x5, x7                  /* reload base: never re-read the literal    */
	ic	iallu                   /* drop any stale I-cache lines              */
	dsb	sy
	isb

2:	/* ---- DIAG: paint GREEN band (rows 750-1199) — relocation done ------
	 * Reached after the FD copy (or already-at-link). About to br into the FD.
	 * If GREEN shows but no BLUE, the EDK II SEC entry faulted before C. */
	movz	x9, #0xEB00
	movk	x9, #0xCA41, lsl #16    /* 0xCA41EB00 = FB + row 750                 */
	movz	w10, #0xE340
	movk	w10, #0x9, lsl #16
	movz	w11, #0xFF00
	movk	w11, #0xFF00, lsl #16   /* 0xFF00FF00 = opaque GREEN                 */
.Lgreen:
	str	w11, [x9], #4
	subs	w10, w10, #1
	b.ne	.Lgreen
	dsb	sy

#if BEYOND0LTE_PRAM_DIAG
	/* ---- DIAG breadcrumb 'B': shim done, about to enter the FD --------- */
	ldr	x12, _pram_base           /* BEYOND0LTE_PRAM_BASE                    */
	mov	w13, #0x42              /* 'B'                                       */
	strb	w13, [x12, #13]         /* text[1] = 'B'                             */
	mov	w13, #2
	str	w13, [x12, #8]          /* size = 2                                  */
	dsb	sy
#endif

	/* ---- Set up a temporary stack before entering EDK II --------------
	 * PeilessSec's ModuleEntryPoint.S calls ArmPlatformPeiBootAction and
	 * ArmEnableVFP BEFORE it programs SP, relying on the entry SP being usable.
	 * sboot may leave SP pointing at a now-invalid/RKP-protected region, which
	 * would fault those pre-stack calls (the "AB but no C" hang). m1-hello always
	 * set SP first; do the same here. The 9810's 0xA0000000 is WRONG here: it
	 * sits inside the VERIFIED camera reservation 0x99000000..0xA7400000
	 * (notes/s10e-root-reads.md:115). 0x83000000 is in the gap FD-end
	 * 0x80780000 .. el2_code 0x87000000 and below both initrd readings
	 * (vendor DT 0x84000000, live 0x89000000) - still UNVERIFIED while K10
	 * (the 9820's write-protected zones) is open. EDK II reprograms SP to its
	 * real stack moments later. */
	movz	x6, #0x0000
	movk	x6, #0x8300, lsl #16    /* x6 = 0x83000000                           */
	mov	sp, x6

	/* ---- Leave the MMU OFF for EDK II ----------------------------------
	 * We deliberately do NOT enable an MMU here. Earlier we did (to dodge
	 * pre-MMU literal-pool load faults), but that was a symptom of a TE-image
	 * stripping misalignment, since fixed properly at build time
	 * (/ALIGN:0x1000 + SEC rule Align=Auto). With the MMU left OFF, EDK II's
	 * MemoryPeim -> ArmConfigureMmu does its own correct break-before-make
	 * `ArmEnableMmu()`; if the shim had left the MMU ON, ArmConfigureMmu sees
	 * ArmMmuEnabled()==TRUE, SKIPS ArmEnableMmu, and just swaps TTBR0 on a live
	 * MMU -> hang (DEVICE-OBSERVED on star2lte: trail stalled at '1', before ArmConfigureMmu
	 * returns). MMU off at handoff is UNVERIFIED on the 9820 (K9); with the MMU off, all RAM is accessible (Device) for our pram breadcrumbs
	 * too. The firmware is built -mstrict-align so it makes no unaligned accesses
	 * before EDK II maps RAM as Normal.
	 *
	 * DIAG breadcrumb 'M' (= shim done its prep, about to enter the FD). */
#if BEYOND0LTE_PRAM_DIAG
	ldr	x12, _pram_base           /* BEYOND0LTE_PRAM_BASE                    */
	add	x12, x12, #12             /* x12 = text start                        */
	ldur	w13, [x12, #-4]          /* w13 = size                              */
	mov	w14, #0x4D              /* 'M'                                       */
	strb	w14, [x12, w13, uxtw]
	add	w13, w13, #1
	stur	w13, [x12, #-4]          /* size += 1                               */
	dsb	sy
#endif

	br	x5                      	/* enter the FD @UEFI_BASE (B -> SEC entry) */

3:	wfe                             /* unreachable; park if the FD ever returns  */
	b	3b

	/* ---- The EDK II FD is concatenated immediately after this label ----
	 * _payload MUST be the last thing emitted so the packaging `cat` places
	 * the FD exactly where `adr x1,_payload` points. */
	.balign	8
_payload:
