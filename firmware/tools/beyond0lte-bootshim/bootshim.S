/* @file
 * bootshim.S - relocating loader that prepends the EDK II firmware device (FD).
 *
 * Mirrors the device-proven edk2-exynos tools/BootShim/BootShim.S flow for exynos9820
 * (https://github.com/sonic011gamer/edk2-exynos): a fake Linux ARM64 Image header, then
 * copy UEFI_SIZE bytes from the appended FD to UEFI_BASE and branch there. Nothing else,
 * except the BEYOND0LTE_SHIM_DIAG block (DECON enable + two white bands, default ON).
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

/* ---- Boot #5 diagnostic (BEYOND0LTE_SHIM_DIAG, default ON; -DBEYOND0LTE_SHIM_DIAG=0 for
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
 * Boot #8 adds the SoC watchdog stop (notes/m8-uefi-image.md B35/B36): every loop of boots
 * #4-#7 was a reset at a fixed ~15.5 s period. Registers:
 *   WDT cl0 0x10050000, cl2 0x10060000  VERIFIED (Exynos9820.h EXYNOS_WDT_BASE; live DTB reg)
 *   WTCON @ +0x00; ENABLE b5, INTEN b2, RSTEN b0  VERIFIED by source: the exynos9820 kernel's
 *     s3c2410_wdt.c :52-61, stop = WTCON &= ~(ENABLE|RSTEN) :461-474 (+ ~INTEN on the
 *     multistage cl2, :~180); compatibles samsung,exynos9-v1-wdt / -v2-wdt match the live DTB.
 * Bands are 200 rows each, WHITE, and each answers one question:
 *   row  200  shim entered
 *   row  500  cl0 WTCON had ENABLE set when we arrived (sboot armed it)
 *   row  800  cl2 WTCON had ENABLE set when we arrived
 *   row 1100  after the stop, BOTH WTCON read back with ENABLE|INTEN|RSTEN clear
 *   row 1850  FD copy done, next instruction is `br` into the FD
 * Uses x9-x13 only (x1/x4/x5/x6 untouched); _band is a leaf (x30). */
#ifndef BEYOND0LTE_SHIM_DIAG
#define BEYOND0LTE_SHIM_DIAG 1
#endif

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
#if BEYOND0LTE_SHIM_DIAG
	movz	x9, #0x1903, lsl #16    /* 0x19030000 = decon_f base               */
	mov	w10, #0x1281             /* prior-art "enable frame buffer writes"  */
	str	w10, [x9]
	dsb	sy
	movz	x9, #0x9400
	movk	x9, #0xCA11, lsl #16    /* FB + row 200                            */
	bl	_band                    /* shim entered                            */

	/* ---- SoC watchdogs: sample, stop, verify ----------------------------- */
	movz	x12, #0x1005, lsl #16   /* 0x10050000 = watchdog_cl0               */
	ldr	w13, [x12]              /* WTCON as sboot left it                  */
	tbz	w13, #5, 1f             /* ENABLE clear -> no band                 */
	movz	x9, #0xF200
	movk	x9, #0xCA2B, lsl #16    /* FB + row 500                            */
	bl	_band                    /* cl0 was armed                           */
1:	movz	x12, #0x1006, lsl #16   /* 0x10060000 = watchdog_cl2               */
	ldr	w13, [x12]
	tbz	w13, #5, 2f
	movz	x9, #0x5000
	movk	x9, #0xCA46, lsl #16    /* FB + row 800                            */
	bl	_band                    /* cl2 was armed                           */
2:	mov	w11, #0x25              /* ENABLE(5) | INTEN(2) | RSTEN(0)         */
	movz	x12, #0x1005, lsl #16
	ldr	w13, [x12]
	bic	w13, w13, w11
	str	w13, [x12]
	movz	x12, #0x1006, lsl #16
	ldr	w13, [x12]
	bic	w13, w13, w11
	str	w13, [x12]
	dsb	sy
	movz	x12, #0x1005, lsl #16
	ldr	w13, [x12]
	movz	x12, #0x1006, lsl #16
	ldr	w9, [x12]
	orr	w13, w13, w9
	tst	w13, w11
	b.ne	3f                       /* a bit survived -> no band               */
	movz	x9, #0xAE00
	movk	x9, #0xCA60, lsl #16    /* FB + row 1100                           */
	bl	_band                    /* both watchdogs read back stopped        */
3:
#endif
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
_entry:
	/* The FD's first word is an ARM64 `B` that GenFv wrote into the FV-header
	 * zero-vector, jumping to the PeilessSec SEC entry (BEYOND0LTE_UEFI.fd
	 * first word 0x1401C800). */
#if BEYOND0LTE_SHIM_DIAG
	movz	x9, #0x9900
	movk	x9, #0xCAA2, lsl #16    /* FB + row 1850                           */
	bl	_band                    /* copy done, entering the FD              */
#endif
	br	x5                      /* enter the FD @UEFI_BASE                 */

_dead:
	b	_dead                   /* never reached                           */

#if BEYOND0LTE_SHIM_DIAG
	/* x9 = first pixel; paints 200 rows x 1440 px of 0xFFFFFFFF. Leaf, no stack. */
_band:
	movz	w10, #0x6500
	movk	w10, #0x4, lsl #16      /* 288000 px = 200 rows x 1440             */
	mov	w11, #0xFFFFFFFF
1:	str	w11, [x9], #4
	subs	w10, w10, #1
	b.ne	1b
	dsb	sy
	ret
#endif

	/* ---- The EDK II FD is concatenated immediately after this label ----
	 * _payload MUST be the last thing emitted so the packaging `cat` places
	 * the FD exactly where `adr x1,_payload` points. */
	.balign	16
_payload:
