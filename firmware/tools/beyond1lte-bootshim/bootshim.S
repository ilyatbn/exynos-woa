/*
 * Minimal Exynos 9820 boot shim for the regular Galaxy S10 (beyond1lte).
 *
 * sboot enters this image using the ARM64 Linux Image protocol. x0 is the
 * physical address of the boot DTB; preserve it unchanged for the platform
 * SEC entry to capture. x1 is used only for the appended firmware payload.
 *
 * No fixed DRAM scratch page, framebuffer writes, watchdog writes, PMIC writes,
 * or S10e memory-map assumptions are permitted here.
 */

#ifndef UEFI_BASE
#error "UEFI_BASE must match the Beyond1Lte FD base address"
#endif
#ifndef UEFI_SIZE
#error "UEFI_SIZE must match the Beyond1Lte FD size"
#endif
#if ((UEFI_BASE & 0xF) != 0)
#error "UEFI_BASE must be 16-byte aligned"
#endif
#if ((UEFI_SIZE & 0xF) != 0)
#error "UEFI_SIZE must be a multiple of 16 bytes"
#endif

        .section .text, "ax"
        .global _head
_head:
        adr     x1, _payload
        b       _start

        /* Fake Linux ARM64 Image header consumed by sboot. */
        .quad   0                       /* text_offset */
        .quad   0                       /* image_size */
        .quad   0                       /* flags */
        .quad   0                       /* res2 */
        .quad   0                       /* res3 */
        .quad   0                       /* res4 */
        .ascii  "ARMd"                  /* magic */
        .long   0                       /* res5 */
        .ascii  "EDK2-MSM"              /* tag */

_uefi_base:
        .quad   UEFI_BASE
_uefi_size:
        .quad   UEFI_SIZE

_start:
        /* No TransferList is supplied by this sboot path. Avoid a stale
         * TPIDRRO_EL0 value surviving a warm reset and being treated as one. */
        msr     tpidrro_el0, xzr
        isb

        /* Clear stale data/unified cache lines from a prior warm boot and
         * invalidate I-cache before copying executable firmware to RAM. */
        mrs     x6, clidr_el1
        ubfx    x7, x6, #24, #3         /* LoC */
        lsl     x7, x7, #1              /* LoC * 2 */
        cbz     x7, 20f
        mov     x10, #0                 /* level * 2 (CSSELR format) */
21:
        add     x2, x10, x10, lsr #1    /* level * 3 */
        lsr     x3, x6, x2
        and     x3, x3, #7              /* cache type */
        cmp     x3, #2
        b.lt    25f                     /* no data or unified cache */
        msr     csselr_el1, x10
        isb
        mrs     x3, ccsidr_el1
        and     x2, x3, #7
        add     x2, x2, #4              /* set shift */
        ubfx    x4, x3, #3, #10         /* max way index */
        clz     w5, w4                  /* way shift */
        ubfx    x8, x3, #13, #15        /* max set index */
22:
        mov     x11, x4
23:
        lsl     x12, x11, x5            /* way */
        orr     x12, x12, x10           /* | level */
        lsl     x13, x8, x2             /* set */
        orr     x12, x12, x13
        dc      cisw, x12
        subs    x11, x11, #1
        b.ge    23b
        subs    x8, x8, #1
        b.ge    22b
25:
        add     x10, x10, #2
        cmp     x7, x10
        b.gt    21b
20:
        dsb     sy
        ic      iallu
        dsb     sy
        isb

        /* Relocate the appended FD. Handle overlap in either direction and
         * leave x0 (the boot DTB pointer) untouched. */
        mov     x4, x1                  /* source */
        ldr     x5, _uefi_base          /* destination */
        ldr     x6, _uefi_size          /* bytes */
        cmp     x5, x4
        b.eq    .Lcopy_done
        b.ls    .Lcopy_forward          /* destination <= source */
        add     x7, x4, x6              /* source end */
        cmp     x5, x7
        b.hs    .Lcopy_forward          /* destination >= source end */

        /* Overlap with destination above source: copy backwards. */
        add     x4, x4, x6
        add     x5, x5, x6
.Lcopy_backward:
        ldp     x2, x3, [x4, #-16]!
        stp     x2, x3, [x5, #-16]!
        subs    x6, x6, #16
        b.ne    .Lcopy_backward
        b       .Lcopy_done

.Lcopy_forward:
1:
        ldp     x2, x3, [x4], #16
        stp     x2, x3, [x5], #16
        subs    x6, x6, #16
        b.ne    1b

.Lcopy_done:
        dsb     sy
        isb
        ldr     x5, _uefi_base
        br      x5

        .balign 16
_payload:
        /* The packaging step appends the FD bytes here. */
