/* @file
 * bootshim.S - relocating loader that prepends the EDK II firmware device (FD).
 *
 * Mirrors the device-proven edk2-exynos tools/BootShim/BootShim.S flow for exynos9820
 * (https://github.com/sonic011gamer/edk2-exynos): a fake Linux ARM64 Image header, then
 * copy UEFI_SIZE bytes from the appended FD to UEFI_BASE and branch there. Nothing else.
 * The 9810 (star2lte) shim added watchdog/cache/paint/SP steps; on the 9820 those fault
 * (boot loop, notes/m8-uefi-image.md B22) - the proven 9820 flow touches no privileged
 * register and writes no unvalidated address, so it is what we ship here.
 *
 * sboot loads this boot.img "kernel" and jumps to offset 0. The EDK II FD (BEYOND0LTE_UEFI.fd)
 * is linked to run at UEFI_BASE, which is NOT where sboot places the loaded image, so the shim
 * is position-independent (adr / br / literal ldr) and relocates the FD to UEFI_BASE at runtime.
 *
 * Layout after packaging:  [ this shim ][ BEYOND0LTE_UEFI.fd ]   (cat'd together)
 *
 * Build: clang -target aarch64-none-elf -ffreestanding -nostdlib -mgeneral-regs-only
 *        -DUEFI_BASE=0x83300000 -DUEFI_SIZE=0x700000 -c bootshim.S -o bootshim.o
 *        ld.lld -o bootshim.elf bootshim.o -Ttext=0x83300000 -e _head --no-relax
 *        llvm-objcopy -O binary bootshim.elf bootshim.bin
 * (The LINK step is required, not optional: a -c-only object can leave the adr/branch
 *  offsets unapplied, which silently breaks the entry. pack-uefi.ps1 links at :92 and
 *  checks the result at :102-107.)
 */

#ifndef UEFI_BASE
#define UEFI_BASE 0x0000000083300000   /* must equal PcdFdBaseAddress (Beyond0LtePkg.dsc / .fdf FD_BASE_ADDRESS). DECISION, not a read: sboot's observed drop for this boot is 0x80118000 and its kernel window ends 0x832D0000 (notes/k4b-drop-address.md K01-K03), so 0x83300000 is the first address above that window that is also below every reservation read (mm_debug 0x91200000) and inside bank0 (ends 0xBAB00000). Nothing has booted here. */
#endif
#ifndef UEFI_SIZE
#define UEFI_SIZE 0x0000000000700000   /* VERIFIED: built BEYOND0LTE_UEFI.fd is exactly 7 340 032 B = 0x700000 */
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
	 * system-register access, no stack, no MMIO - the proven 9820 flow has none. */
_start:
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
	br	x5                      /* enter the FD @UEFI_BASE                 */

_dead:
	b	_dead                   /* never reached                           */

	/* ---- The EDK II FD is concatenated immediately after this label ----
	 * _payload MUST be the last thing emitted so the packaging `cat` places
	 * the FD exactly where `adr x1,_payload` points. */
	.balign	16
_payload:
