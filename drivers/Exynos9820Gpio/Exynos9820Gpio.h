//
// Exynos9820Gpio.h - register map + driver context for the Exynos 9820
// "ALIVE" GPIO controller (pinctrl@15850000), a GpioClx provider.
//
// Every register offset and encoding here is VERIFIED from the vendor
// pinctrl driver (LineageOS/android_kernel_samsung_exynos9820,
// drivers/pinctrl/samsung/pinctrl-exynos-arm64.c + pinctrl-exynos.c +
// pinctrl-exynos.h) and the shipped DTB; the full derivation with sources
// is in notes/d8-audio.md §3. UNVERIFIED items are named as such.
//
// BANKING MODEL (important): the Exynos ALIVE block has ONE GIC interrupt
// per pin (EINT0..33), while GpioClx assigns _CRS interrupt entries to
// BANKS in order (learn.microsoft.com "General-Purpose I/O (GPIO)": "the
// first interrupt listed is assigned to bank 0, the next one listed is
// assigned to bank 1"). To reconcile the two, this driver presents the
// controller to GpioClx as 34 banks of ONE pin each: pseudo-bank B == pin
// B == EINT B == the B-th Interrupt entry in the DSDT _CRS. The physical
// 4-bit/8-bit/1-bit register fields are recovered internally as
// word = B / 8, bitfield = B % 8. GpioClx explicitly allows any pin count
// per bank (CLIENT_CONTROLLER_BASIC_INFORMATION remarks).
//
// Consequence: GpioClx's per-bank interrupt lock does NOT serialize RMWs
// of a shared physical word (pseudo-banks 0..7 share one word), so every
// register RMW in the .c takes the driver's own global spin lock.
//

#pragma once

//
// Pin space (GpioClx-linear; equals the DSDT _CRS Interrupt order):
//   pins/banks 0..7    gpa0-0..7  (EINT0..7,   SPI 5..12,  GSIV 37..44)
//   pins/banks 8..15   gpa1-0..7  (EINT8..15,  SPI 13..20, GSIV 45..52)
//   pins/banks 16..23  gpa2-0..7  (EINT16..23, SPI 21..28, GSIV 53..60)
//   pins/banks 24..31  gpa3-0..7  (EINT24..31, SPI 29..36, GSIV 61..68)
//   pins/banks 32..33  gpa4-0..1  (EINT32..33, SPI 37..38, GSIV 69..70)
//
#define EXYNOS_GPIO_TOTAL_PINS    34
#define EXYNOS_GPIO_PINS_PER_BANK 1      // pseudo-bank model, see header
#define EXYNOS_GPIO_BANK_STRIDE   8      // physical pins per register word

//
// Per-physical-bank registers (bank type exynos9810_bank_type_alive:
// fld_width {4,1,4,4}, reg_offset {0x00,0x04,0x08,0x0C}).
//
#define EXYNOS_BANK_CON           0x000  // 4 bits/pin: pin function (0=in,
                                         // 1=out, ..., 0xF = external int)
#define EXYNOS_BANK_DAT           0x004  // 1 bit/pin: level
#define EXYNOS_BANK_PUD           0x008  // 4 bits/pin: 0 none, 1 down,
                                         // 2 up, 3 hi-Z (Exynos convention,
                                         // vendor pinctrl bindings)
#define EXYNOS_BANK_PDN           0x00C  // 4 bits/pin: power-down config
                                         // (never touched here)

// Physical bank register bases inside the 0x15850000+0x1000 window
// (VERIFIED: exynos9820_pin_banks0).
//
static const ULONG ExynosGpioPhysBankBase[5] = {
    0x000,  // gpa0
    0x020,  // gpa1
    0x040,  // gpa2
    0x060,  // gpa3
    0x080,  // gpa4
};

// Valid pin count per PHYSICAL bank (VERIFIED: nr_pins, same table).
//
static const ULONG ExynosGpioPhysBankPins[5] = { 8, 8, 8, 8, 2 };

//
// Wakeup-EINT block (child compatible "samsung,exynos7-wakeup-eint" ->
// exynos7_wkup_irq_chip: ECON 0x700, EMASK 0x900, EPEND 0xA00; EFLTCON
// 0x800). One word per PHYSICAL bank:
//   EINT_CON   0x700 + 4*physbank, 4-bit field/pin (trigger type)
//   EINT_FLTCON 0x800 + 8*physbank, 8-bit field/pin; FLTCON0 = pins 0-3,
//                FLTCON1 = +4 = pins 4-7
//   EINT_MASK  0x900 + 4*physbank, 1 bit/pin (1 = masked)
//   EINT_PEND  0xA00 + 4*physbank, 1 bit/pin (write-1-clears)
// (VERIFIED: EXYNOS7_WKUP_ECON/EMASK/EPEND + EXYNOS_GPIO_EFLTCON offsets,
// the eint_offset/fltcon_offset columns, exynos_eint_flt_config.)
//
#define EXYNOS_EINT_CON_BASE      0x700
#define EXYNOS_EINT_FLTCON_BASE   0x800
#define EXYNOS_EINT_MASK_BASE     0x900
#define EXYNOS_EINT_PEND_BASE     0xA00

// Trigger-type encodings for the 4-bit EINT_CON field
// (VERIFIED: EXYNOS_EINT_LEVEL_LOW..EDGE_BOTH in pinctrl-exynos.h).
//
#define EXYNOS_EINT_TYPE_LEVEL_LOW   0
#define EXYNOS_EINT_TYPE_LEVEL_HIGH  1
#define EXYNOS_EINT_TYPE_EDGE_FALL   2
#define EXYNOS_EINT_TYPE_EDGE_RISE   3
#define EXYNOS_EINT_TYPE_EDGE_BOTH   4

// FLTCON field (VERIFIED: EXYNOS_EINT_FLTCON_EN/SEL/WIDTH; the vendor
// programs EN|SEL width 0 for every gpa bank, and the validated 9810
// touch driver writes the same 0xC0).
//
#define EXYNOS_EINT_FLTCON_EN     0x80
#define EXYNOS_EINT_FLTCON_SEL    0x40
#define EXYNOS_EINT_FLTCON_VALUE  (EXYNOS_EINT_FLTCON_EN | EXYNOS_EINT_FLTCON_SEL)

// Pin-function values in the CON field (VERIFIED: DT uses 0xF for EINT
// pins; 0 = input, 1 = output per the Exynos pinctrl binding).
//
#define EXYNOS_PINFUNC_INPUT      0
#define EXYNOS_PINFUNC_OUTPUT     1
#define EXYNOS_PINFUNC_EINT       0xF

//
// Driver context (allocated by GpioClx: ControllerContextSize).
//
typedef struct _EXYNOS_GPIO_CONTEXT {

    PUCHAR       Registers;     // mapped 0x15850000 window
    ULONG        RegisterCount; // mapped length
    BOOLEAN      Started;
    WDFSPINLOCK  HwLock;        // guards every register RMW (see header)

} EXYNOS_GPIO_CONTEXT, *PEXYNOS_GPIO_CONTEXT;
