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
 * Boot #8 sampled both SoC watchdogs: neither was armed (notes/m8-uefi-image.md B38), so that
 * probe is gone. Boot #9 showed the FD dies before its FIRST C call (B41) although the FD bytes
 * are right offline, so boot #10 reads the CPU state and checks the copy, and shows each value
 * as a 32-cell BINARY STRIP (MSB left, cell = 45 px; full block = 1, thin underline = 0):
 *   row  200  band: shim entered
 *   row  500  strip: runtime address of _head (where sboot put us), low 32 bits
 *   row  650  strip: CurrentEL (bits 3:2 = EL)
 *   row  800  strip: SCTLR_EL2 if at EL2 else SCTLR_EL1 (bit0 M = MMU, bit2 C, bit12 I)
 *   row  950  strip: first word read back at UEFI_BASE after the copy (built FD: 0x1401CC00)
 *   row 1100  strip: number of 8-byte words that differ between source and UEFI_BASE (want 0)
 *   row 1850  band: next instruction is `br` into the FD
 * Uses x2/x3 (as the copy does), x7-x17; x0, x1, x4-x6 kept for the copy and the FD. */
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

	/* ---- CPU state readout --------------------------------------------- */
	adr	x12, _head
	movz	x9, #0xF200
	movk	x9, #0xCA2B, lsl #16    /* FB + row 500                            */
	bl	_bits                    /* where sboot loaded us                   */
	mrs	x12, CurrentEL
	movz	x9, #0x2100
	movk	x9, #0xCA39, lsl #16    /* FB + row 650                            */
	bl	_bits                    /* CurrentEL                               */
	mrs	x12, CurrentEL
	cmp	x12, #8                 /* EL2 = 0b10 << 2                         */
	b.ne	4f
	mrs	x12, SCTLR_EL2
	b	5f
4:	mrs	x12, SCTLR_EL1
5:	movz	x9, #0x5000
	movk	x9, #0xCA46, lsl #16    /* FB + row 800                            */
	bl	_bits                    /* SCTLR of the current EL                 */
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
#if BEYOND0LTE_SHIM_DIAG
	ldr	w12, [x5]               /* first FD word as it now reads at dst    */
	movz	x9, #0x7F00
	movk	x9, #0xCA53, lsl #16    /* FB + row 950                            */
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
	movz	x9, #0xAE00
	movk	x9, #0xCA60, lsl #16    /* FB + row 1100                           */
	bl	_bits                    /* copy mismatch count                     */
#endif
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

	/* x9 = FB address of the strip's top-left, w12 = value. 32 cells of 45 px (40 painted),
	 * MSB first; a 1 bit is an 80-row block, a 0 bit a 4-row underline at the block's
	 * bottom. Leaf, no stack; clobbers w7, w8, w10, w11, w13-w17. */
_bits:
	mov	w13, #31
	mov	w8, #-1
7:	lsr	w14, w12, w13
	mov	w15, #31
	sub	w15, w15, w13           /* cell index, MSB = 0                     */
	mov	w16, #180               /* 45 px * 4 B                             */
	umaddl	x17, w15, w16, x9
	tbnz	w14, #0, 8f
	movz	x16, #0xAE00
	movk	x16, #0x6, lsl #16      /* 76 rows * 5760 B                        */
	add	x17, x17, x16
	mov	w10, #4
	b	9f
8:	mov	w10, #80
9:	mov	w11, #40
	mov	x7, x17
10:	str	w8, [x7], #4
	subs	w11, w11, #1
	b.ne	10b
	mov	x16, #5760
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
