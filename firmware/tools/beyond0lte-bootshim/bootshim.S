/* @file
 * bootshim.S - relocating loader that prepends the EDK II firmware device (FD).
 *
 * Mirrors the device-proven edk2-exynos tools/BootShim/BootShim.S flow for exynos9820
 * (https://github.com/sonic011gamer/edk2-exynos): a fake Linux ARM64 Image header, then
 * copy UEFI_SIZE bytes from the appended FD to UEFI_BASE and branch there. Nothing else,
 * except the BEYOND0LTE_SHIM_DIAG block (white bands + bit strips, default OFF since 2026-10-11; the DECON enable is unconditional).
 * The 9810 (star2lte) shim added watchdog/cache/paint/SP steps; with those, boot #4 looped
 * (notes/m8-uefi-image.md B20-B22). Which step faulted is NOT known - they are dropped as
 * suspects, because the proven 9820 flow touches no system register and no MMIO.
 *
 * sboot loads this boot.img "kernel" and jumps to offset 0. The EDK II FD (BEYOND0LTE_UEFI.fd)
 * is linked to run at UEFI_BASE, which is NOT where sboot places the loaded image, so the shim
 * is position-independent (adr / br / literal ldr) and relocates the FD to UEFI_BASE at runtime.
 *
 * Layout after packaging:  [ this shim ][ BEYOND0LTE_UEFI.fd ]   (cat'd together)
 *
 * Build: clang -target aarch64-none-elf -ffreestanding -nostdlib -mgeneral-regs-only
 *        -DUEFI_BASE=0x90000000 -DUEFI_SIZE=0x700000 -c bootshim.S -o bootshim.o
 *        ld.lld -o bootshim.elf bootshim.o -Ttext=0x90000000 -e _head --no-relax
 *        llvm-objcopy -O binary bootshim.elf bootshim.bin
 * (The LINK step is required, not optional: a -c-only object can leave the adr/branch
 *  offsets unapplied, which silently breaks the entry. pack-uefi-beyond0lte.ps1 links at
 *  :91 and checks the result at :101-103.)
 */

#ifndef UEFI_BASE
#define UEFI_BASE 0x0000000090000000   /* must equal PcdFdBaseAddress (Beyond0LtePkg.dsc / .fdf FD_BASE_ADDRESS). PRIOR ART, not a read: edk2-exynos configs/exynos9820.conf FD_BASE=0x90000000 (notes/m8-uefi-image.md B23/B25); was the DECISION 0x83300000. Not booted on this phone yet. */
#endif
#ifndef UEFI_SIZE
#define UEFI_SIZE 0x0000000000700000   /* VERIFIED: built BEYOND0LTE_UEFI.fd is exactly 7 340 032 B = 0x700000 */
#endif

/* ---- Boot #5 diagnostic (BEYOND0LTE_SHIM_DIAG, default OFF since 2026-10-11 - see below; -DBEYOND0LTE_SHIM_DIAG=0 for
 * the exact prior-art flow). The only deviation from edk2-exynos BootShim.S. Boot #4 showed
 * nothing on the panel, which proves nothing: the panel only shows CPU stores to the scanout
 * buffer once DECON is told to (SimpleFramebufferDxe.c:155-161), and edk2-exynos's 9820
 * PlatformInitialize() does exactly one write for it: MmioWrite32(0x19030000, 0x1281)
 * "Enable Frame Buffer Writes" (Exynos9820Pkg/Library/PlatformPrePiLib/PlatformUtils.c).
 *   DECON base 0x19030000  VERIFIED   (Exynos9820.h EXYNOS_DECON_F_BASE, decon_f@0x19030000)
 *   value 0x1281 @ +0x0    PRIOR ART  (bit meaning UNVERIFIED on this phone)
 *   FB 0xCA000000          VERIFIED   (Exynos9820.h EXYNOS_FB_BASE, s3cfb.bootloaderfb)
 *   stride 1440*4 B        UNVERIFIED (pixel format never read; bands are all-ones WHITE so
 *                                       the colour does not depend on the format)
 * The S10e panel is 1080 x 2280 at 4 B/px (stock DTBO s6e3fa7_beyond0, notes/m8-uefi-image.md
 * B44) - boots #5-#10 drew with the S10's 1440 stride, so rows past ~1710 never reached the panel.
 * Geometry here: line = 4320 B, FB_ROW(r) = 0xCA000000 + r * 4320.
 * Boot #11 layout (rows):
 *   150-299   band: shim entered
 *   340       strip: runtime address of _head (where sboot put us), low 32 bits
 *   450       strip: CurrentEL (bits 3:2)
 *   560       strip: SCTLR_EL2 if at EL2 else SCTLR_EL1
 *   670       strip: CNTFRQ_EL0 (architected timer frequency; T05)
 *   1000-1039 bar: 108 segments filled over ~10 s of CNTVCT time BEFORE the copy - if something
 *             resets the phone a fixed time after sboot jumps, the bar stops at that time
 *   780       strip: first word read back at UEFI_BASE after the copy (built FD: 0x1401CC00)
 *   890       strip: count of 8-byte words that differ src vs UEFI_BASE (want 0)
 *   1100-1199 band: next instruction is `br` into the FD
 * Strips: 32 cells x 33 px (28 painted), MSB left; 1 = 80-row block, 0 = 4-row underline.
 * Uses x2/x3 (as the copy does), x7-x17; x0, x1, x4-x6 kept for the copy and the FD. */
#define FB_ROW(r)  (0xCA000000 + (r) * 4320)

/* The 10 s time bar answered its question in boot #11 (no fixed-time reset, notes
 * /m8-uefi-image.md B47) and costs 10 s per boot, so it is off by default (B73). */
#ifndef BEYOND0LTE_SHIM_TIMEBAR
#define BEYOND0LTE_SHIM_TIMEBAR 0
#endif
#ifndef BEYOND0LTE_SHIM_DIAG
/* 2026-10-11 (user: "get rid of the white lines the shim adds to the samsung logo"): off. The bands/bit
 * strips predate the RAM boot log + progress words (0x51-0x56); -DBEYOND0LTE_SHIM_DIAG=1 brings them back.
 * The DECON framebuffer-write enable is no longer part of this switch. */
#define BEYOND0LTE_SHIM_DIAG 0
#endif

	/* x = FB_ROW(row), two instructions, no literal pool. */
	.macro	LOADFB reg, row
	movz	\reg, #((FB_ROW(\row)) & 0xFFFF)
	movk	\reg, #((FB_ROW(\row)) >> 16), lsl #16
	.endm

	.text
	.global _head
_head:
	adr	x1, _payload            /* x1 = runtime address of the appended FD   */
	b	_start                  /* (this pair is the header's code0/code1)   */

	/* ---- Fake Linux ARM64 Image header (what sboot reads) ----------------
	 * text_offset/image_size/flags are ZERO, exactly as the proven edk2-exynos
	 * shim ("has to be 0 on some devices"). The copy destination/size live in
	 * the _uefi_base/_uefi_size quads after the tag, not in this header. */
	.quad	0                       /* text_offset                           */
	.quad	0                       /* image_size                            */
	.quad	0                       /* flags                                 */
	.quad	0                       /* res2                                  */
	.quad	0                       /* res3                                  */
	.quad	0                       /* res4                                  */
	.ascii	"ARMd"               /* magic = 0x644d5241                    */
	.long	0                       /* res5 (PE/COFF header offset; none)    */
	.ascii	"EDK2-MSM"              /* tag, kept from the proven shim        */

_uefi_base:
	.quad	UEFI_BASE               /* copy destination                      */
_uefi_size:
	.quad	UEFI_SIZE               /* byte count (multiple of 16)           */

	/* ---- Relocate the FD to UEFI_BASE, then branch into it --------------
	 * Instruction-for-instruction the proven _Start/_CopyLoop/_Entry. x4 = src
	 * (the appended FD), x5 = dst, x6 = bytes left. The copy post-increments
	 * x5, so the base is reloaded before the branch. No cache maintenance, no
	 * system-register access, no stack. The only MMIO/stores outside the copy are
	 * the BEYOND0LTE_SHIM_DIAG block (off = the exact prior-art flow). */
_start:
	/* ---- W28 boot progress word (RAM, survives the watchdog reset) -------
	 * 0x8FE0F000: [0] = the previous boot's last code, [1] = this boot's code. Shim
	 * 0x51..0x56, SEC 0x60, BootLogDxe 0x70/0x71. Inside the reserved Windows-PRAM MiB
	 * (Beyond0LteBootLog.h) and inside the 64 KB saved as WINPRAM.BIN. */
	.macro	PROGRESS code
	movz	x16, #0x8FE0, lsl #16
	movk	x16, #0xF000
	mov	w17, #\code
	str	w17, [x16, #4]
	dsb	sy
	.endm
	movz	x16, #0x8FE0, lsl #16
	movk	x16, #0xF000
	ldr	w17, [x16, #4]
	str	w17, [x16]              /* prev = last boot's code                   */
	PROGRESS 0x51                   /* shim entered                              */

	/* ---- W34: THE second-boot hang. PeilessSec's _ModuleEntryPoint takes the
	 * TransferList base from TPIDRRO_EL0 and only initialises that register when x1
	 * holds the TL magic, which ours never does. TPIDRRO_EL0 survives a warm reset:
	 * after Windows (which keeps per-thread data there) or anything else left it
	 * non-zero, SecMain dereferenced the stale value as a TransferList right after
	 * the MMU came on (progress 0x63) and faulted with no vectors -> hang. After
	 * Linux it happened to be 0, which is why boots following TWRP always worked.
	 * Zero it: "no TransferList". */
	msr	tpidrro_el0, xzr
	isb

	/* ---- W25 dead man's switch (unattended bring-up) ---------------------
	 * Reboot reason = RECOVERY (PMU INFORM3 0x1586080C = 0x12345674) and watchdog cl0 =
	 * 82 s, before anything else can hang. The firmware sets the reason back to normal
	 * and re-arms at ReadyToBoot / ExitBootServices. Constants: Exynos9820.h, verified
	 * from the stock kernel + sboot (notes/s10e-reboot-wdt-constants.md). x0/x1 kept. */
	movz	x9, #0x1586, lsl #16    /* PMU                                       */
	movz	w10, #0x5674
	movk	w10, #0x1234, lsl #16   /* 0x12345674 = reason base | recovery (4)  */
	mov	x11, #0x80C              /* INFORM3                                  */
	str	w10, [x9, x11]
	movz	x12, #0x1005, lsl #16   /* watchdog cl0                             */
	str	wzr, [x12]              /* WTCON = 0 (stop)                          */
	mov	w10, #1
	str	w10, [x12, #0xC]        /* WTCLRINT                                  */
	mov	w10, #2                 /* NONCPU_INT_EN bit index                   */
	movz	x11, #0xD244            /* 0x1244 | 0xC000 atomic-set alias           */
	str	w10, [x9, x11]
	movz	w10, #0xFE27            /* 82 s at 26 MHz / 128 / 256                */
	str	w10, [x12, #4]          /* WTDAT                                     */
	str	w10, [x12, #8]          /* WTCNT                                     */
	movz	w10, #0xFF39            /* prescaler 0xFF, DIV128, RSTEN, EN         */
	str	w10, [x12]              /* WTCON                                     */
	dsb	sy
	PROGRESS 0x52                   /* watchdog armed                            */

	/* ---- W25 clean+invalidate every data/unified cache level by set/way, then
	 * the I-cache. A reset that our firmware triggers (PMIC keys, watchdog, SWRESET)
	 * does not flush first, and the shim runs with the MMU and caches off: lines from
	 * the previous boot (same addresses: FD at 0x90000000, UEFI RAM) would otherwise
	 * shadow the fresh copy once SEC enables the caches. Suspected cause of the
	 * "second boot after our firmware hangs on the shim" behaviour (user, 2026-10-10).
	 * Classic CCSIDR layout. Uses x2-x8, x10-x13; x0/x1 kept. */
	mrs	x6, clidr_el1
	ubfx	x7, x6, #24, #3         /* LoC                                       */
	lsl	x7, x7, #1              /* LoC * 2                                   */
	cbz	x7, 20f
	mov	x10, #0                 /* level * 2 (CSSELR format)                 */
21:	add	x2, x10, x10, lsr #1    /* level * 3                                 */
	lsr	x3, x6, x2
	and	x3, x3, #7              /* cache type at this level                  */
	cmp	x3, #2
	b.lt	25f                     /* no data or unified cache                  */
	msr	csselr_el1, x10
	isb
	mrs	x3, ccsidr_el1
	and	x2, x3, #7
	add	x2, x2, #4              /* set shift = log2(line bytes)              */
	ubfx	x4, x3, #3, #10         /* max way index                             */
	clz	w5, w4                  /* way shift                                 */
	ubfx	x8, x3, #13, #15        /* max set index                             */
22:	mov	x11, x4
23:	lsl	x12, x11, x5            /* way                                       */
	orr	x12, x12, x10           /* | level                                   */
	lsl	x13, x8, x2             /* set                                       */
	orr	x12, x12, x13
	dc	cisw, x12
	subs	x11, x11, #1
	b.ge	23b
	subs	x8, x8, #1
	b.ge	22b
25:	add	x10, x10, #2
	cmp	x7, x10
	b.gt	21b
20:	dsb	sy
	ic	iallu
	dsb	sy
	isb
	PROGRESS 0x53                   /* caches cleaned                            */

	/* DECON "enable frame buffer writes" - always on: the UEFI progress bar and the OS framebuffer
	 * depend on CPU stores reaching the panel (only the white bands below are diagnostics). */
	movz	x9, #0x1903, lsl #16    /* 0x19030000 = decon_f base               */
	mov	w10, #0x1281             /* prior-art "enable frame buffer writes"  */
	str	w10, [x9]
	dsb	sy
#if BEYOND0LTE_SHIM_DIAG
	LOADFB	x9, 150
	movz	w10, #0x78D0
	movk	w10, #0x2, lsl #16      /* 162000 px = 150 rows x 1080             */
	bl	_band                    /* shim entered                            */

	adr	x12, _head
	LOADFB	x9, 340
	bl	_bits                    /* where sboot loaded us                   */
	mrs	x12, CurrentEL
	LOADFB	x9, 450
	bl	_bits                    /* CurrentEL                               */
	mrs	x12, CurrentEL
	cmp	x12, #8                 /* EL2 = 0b10 << 2                         */
	b.ne	4f
	mrs	x12, SCTLR_EL2
	b	5f
4:	mrs	x12, SCTLR_EL1
5:	LOADFB	x9, 560
	bl	_bits                    /* SCTLR of the current EL                 */
	mrs	x12, CNTFRQ_EL0
	LOADFB	x9, 670
	bl	_bits                    /* timer frequency                         */

#if BEYOND0LTE_SHIM_TIMEBAR
	/* ---- 10 s time bar (CNTVCT: never trapped at EL1) ------------------ */
	mrs	x15, CNTFRQ_EL0
	mov	x16, #11
	udiv	x15, x15, x16           /* ticks per segment ~ 1/11 s              */
	mrs	x13, CNTVCT_EL0         /* t0                                      */
	mov	x14, #0                 /* segment index                           */
	LOADFB	x9, 1000
11:	add	x16, x14, #1
	mul	x16, x16, x15
	add	x16, x16, x13           /* deadline for this segment               */
12:	mrs	x17, CNTVCT_EL0
	cmp	x17, x16
	b.lo	12b
	mov	x16, #40                /* segment = 8 px x 40 rows at x = idx*10  */
	add	x7, x9, x14, lsl #5     /* idx * 40 B (10 px)                      */
	add	x7, x7, x14, lsl #3
13:	mov	w8, #-1
	str	w8, [x7]
	str	w8, [x7, #4]
	str	w8, [x7, #8]
	str	w8, [x7, #12]
	str	w8, [x7, #16]
	str	w8, [x7, #20]
	str	w8, [x7, #24]
	str	w8, [x7, #28]
	mov	x17, #4320
	add	x7, x7, x17
	subs	x16, x16, #1
	b.ne	13b
	dsb	sy
	add	x14, x14, #1
	cmp	x14, #108
	b.ne	11b
#endif /* BEYOND0LTE_SHIM_TIMEBAR */
#endif
	PROGRESS 0x54                   /* diag done, copying                        */
	mov	x4, x1                  /* x4 = source (FD runtime address)        */
	ldr	x5, _uefi_base          /* x5 = UEFI_BASE (destination)            */
	cmp	x4, x5
	b.eq	_entry                  /* already at the link address             */
	ldr	x6, _uefi_size          /* x6 = byte count                         */
_copyloop:
	ldp	x2, x3, [x4], #16
	stp	x2, x3, [x5], #16
	subs	x6, x6, #16
	b.ne	_copyloop
	ldr	x5, _uefi_base          /* dst was advanced by the copy            */
#if BEYOND0LTE_SHIM_DIAG
	ldr	w12, [x5]               /* first FD word as it now reads at dst    */
	LOADFB	x9, 780
	bl	_bits
	mov	x13, x1                 /* src                                     */
	mov	x14, x5                 /* dst                                     */
	ldr	x15, _uefi_size
	mov	x12, #0                 /* mismatching 8-byte words                */
6:	ldr	x2, [x13], #8
	ldr	x3, [x14], #8
	cmp	x2, x3
	cinc	x12, x12, ne
	subs	x15, x15, #8
	b.ne	6b
	LOADFB	x9, 890
	bl	_bits                    /* copy mismatch count                     */
#endif
_entry:
	/* The FD's first word is an ARM64 `B` that GenFv wrote into the FV-header
	 * zero-vector, jumping to the PeilessSec SEC entry (BEYOND0LTE_UEFI.fd
	 * first word 0x1401CC00 at FD base 0x90000000). */
#if BEYOND0LTE_SHIM_DIAG
	LOADFB	x9, 1100
	movz	w10, #0xA5E0
	movk	w10, #0x1, lsl #16      /* 108000 px = 100 rows x 1080             */
	bl	_band                    /* copy done, entering the FD              */
#endif
	PROGRESS 0x56                   /* branching into the FD                     */
	ldr	x5, _uefi_base
	br	x5                      /* enter the FD @UEFI_BASE                 */

_dead:
	b	_dead                   /* never reached                           */

#if BEYOND0LTE_SHIM_DIAG
	/* x9 = first pixel, w10 = pixel count; paints 0xFFFFFFFF. Leaf, no stack. */
_band:
	mov	w11, #-1
1:	str	w11, [x9], #4
	subs	w10, w10, #1
	b.ne	1b
	dsb	sy
	ret

	/* x9 = FB address of the strip's top-left, w12 = value. 32 cells of 33 px (28 painted),
	 * MSB first; a 1 bit is an 80-row block, a 0 bit a 4-row underline at the block's
	 * bottom. Leaf, no stack; clobbers w7, w8, w10, w11, w13-w17. */
_bits:
	mov	w13, #31
	mov	w8, #-1
7:	lsr	w14, w12, w13
	mov	w15, #31
	sub	w15, w15, w13           /* cell index, MSB = 0                     */
	mov	w16, #132               /* 33 px * 4 B                             */
	umaddl	x17, w15, w16, x9
	tbnz	w14, #0, 8f
	movz	x16, #0x0280
	movk	x16, #0x5, lsl #16      /* 76 lines * 4320 B                       */
	add	x17, x17, x16
	mov	w10, #4
	b	9f
8:	mov	w10, #80
9:	mov	w11, #28
	mov	x7, x17
10:	str	w8, [x7], #4
	subs	w11, w11, #1
	b.ne	10b
	mov	x16, #4320
	add	x17, x17, x16
	subs	w10, w10, #1
	b.ne	9b
	subs	w13, w13, #1
	b.ge	7b
	dsb	sy
	ret
#endif

	/* ---- The EDK II FD is concatenated immediately after this label ----
	 * _payload MUST be the last thing emitted so the packaging `cat` places
	 * the FD exactly where `adr x1,_payload` points. */
	.balign	16
_payload:
