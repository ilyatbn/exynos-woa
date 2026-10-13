/*  Exynos9820Bt.h — S10e (beyond0lte) BCM4375 Bluetooth-over-UART transport:
    constants and device state.

    HARDWARE CONSTANTS carry the same VERIFIED/UNVERIFIED discipline as
    firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h (the
    "Bluetooth" block there is the authoritative copy; these mirror what the
    driver reads). Protocol constants are cited to open references used as
    REFERENCE DOCUMENTS only (license posture: README.md):
      hci_bcm.c / btbcm.c  = mainline Linux (GPL-2.0) — behavior reference
      serialhcibus / cywbtserialbus = MS-PL — architecture + BthxDDI shape

    NOTHING in this driver is validated on an S10e yet. The rung ladder
    (R1..R6) is defined in docs/exynos9820-port/D4c-BT-DESIGN.md section 1.
*/

#ifndef _EXYNOS9820_BT_H
#define _EXYNOS9820_BT_H

#include <ntddk.h>
#include <wdf.h>
#include <bthxddi.h>

/* ----------------------------------------------------------------------
 * ACPI-described device (DSDT Device (BTH0)). The UEFI half latches
 * CMU_PERIC1 gates, the gpq0 pin mux and the USI mode register
 * (0x10821000 = 1); this driver maps ONLY what _CRS hands it and
 * re-asserts the USI quirks that live INSIDE the UART window.
 * -------------------------------------------------------------------- */

#define BT_ACPI_HARDWARE_ID       L"ACPI\\SAMS9826"   /* must match DSDT _HID */

/* _CRS ORDER IS A BINDING CONTRACT (Dsdt.asl, Device (BTH0)):
 *   descriptor 0 = UART Memory32Fixed
 *   descriptor 1 = GpioInt (bt_hostwake, gpa2-3 / EINT19)
 *   Interrupt resource list entry 0 = UART IRQ (GSIV 408)            */
#define BT_RES_UART_WINDOW        0
#define BT_RES_HOSTWAKE_GPIO      1

/* ----------------------------------------------------------------------
 * Samsung/Exynos UART register map (VERIFIED: same offsets as the repo's
 * Exynos9820SerialPortLib + vendor include/linux/serial_s3c.h; window is
 * 0x100 bytes, base 0x10840000 per base.dtb).
 * -------------------------------------------------------------------- */
#define BT_UART_ULCON             0x00
#define BT_UART_UCON              0x04
#define BT_UART_UFCON             0x08
#define BT_UART_UTRSTAT           0x10
#define BT_UART_UFSTAT            0x18
#define BT_UART_UTXH              0x20
#define BT_UART_URXH              0x24
#define BT_UART_UBRDIV            0x28   /* VERIFIED (serial_s3c.h:38)      */
#define BT_UART_UFRACVAL          0x2C   /* UNVERIFIED encoding (DIVSLOT at
                                            serial_s3c.h:210; fractional
                                            baud math closed by rung R1)   */
#define BT_UART_USI_CON           0xC4   /* VERIFIED (serial_s3c.h:46)      */
#define BT_UART_USI_OPT           0xC8   /* VERIFIED (serial_s3c.h:47)      */

#define BT_ULCON_8N1              0x3             /* VERIFIED (SerialPortLib) */
#define BT_UCON_INT_MODE          ((1u<<9)|(1u<<8)|(1u<<2)|(1u<<0)|(1u<<10))
                                    /* TXILEVEL|RXILEVEL|TXIRQMODE|RXIRQMODE|
                                       UCLK — VERIFIED (serial_s3c.h:100-106) */
#define BT_UFCON_FIFO_EN          (1u<<0)         /* VERIFIED (S3C2410_UFCON_FIFOMODE) */
#define BT_UTRSTAT_TX_EMPTY       (1u<<1)         /* VERIFIED (SerialPortLib) */
#define BT_UTRSTAT_RX_READY       (1u<<0)         /* VERIFIED (SerialPortLib) */
#define BT_USI_SET_RESET          1               /* VERIFIED (samsung.c
                                     exynos_usi_init: write 1 then 0)       */
#define BT_USI_CLKREQ_ON          (1u<<1)         /* VERIFIED (USI_OPTION bit1) */

/* Baud generation: UBRDIV = src/(16*baud) - 1. The PERIC1 UART source clock
 * is NOT yet read — 100 MHz is the working assumption, closed by the R1
 * loopback rung (wrong clock shows as garbage echo, then bisect).        */
#define BT_UART_SRC_CLK_HZ        100000000u      /* UNVERIFIED (R1 closes)  */
#define BT_UART_INIT_BAUD         115200u         /* VERIFIED convention for
                                     Broadcom ROM download state            */

/* ----------------------------------------------------------------------
 * H4 framing (HCI UART transport spec; shape VERIFIED against hci_h4.c)
 * -------------------------------------------------------------------- */
#define H4_COMMAND                0x01
#define H4_ACL                    0x02
#define H4_SCO                    0x03
#define H4_EVENT                  0x04

/* Minimal HCI headers (Bluetooth Core Spec vol 4 part E; widths VERIFIED
 * against struct hci_command_hdr usage in btbcm.c:213-250). */
#define HCI_HDR_COMMAND_SZ        3   /* opcode u16 LE + plen u8 */

/* ----------------------------------------------------------------------
 * Broadcom vendor opcodes (facts from hci_bcm.c/btbcm.c + the device .hcd;
 * GPL sources referenced for semantics only, no code copied)
 * -------------------------------------------------------------------- */
#define BCM_HCI_OP_SET_BAUDRATE   0xFC18  /* u16 zero + u32 baud (hci_bcm.c) */
#define BCM_HCI_OP_SET_UART_CLOCK 0xFC45  /* 1-byte clock type, >3 Mbaud     */
#define BCM_HCI_OP_START_DOWNLOAD 0xFC2E  /* btbcm_patchram start            */
#define BCM_HCI_OP_WRITE_RAM      0xFC4C  /* .hcd record: addr u32 + data    */
#define BCM_HCI_OP_LAUNCH_RAM     0xFC4E  /* .hcd last record: addr 0xFFFFFFFF */
#define HCI_OP_READ_LOCAL_VERSION 0x1001

/* Patch blob placed by the installer from the device's own /vendor/firmware
 * (NEVER committed — M12 blob policy). Name UNVERIFIED until the live read;
 * default from the extracted vendor image (notes/bt-driver.md B1). */
#define BT_FW_DIR                 L"\\??\\C:\\S9WOA\\bt\\"
#define BT_FW_FILE                L"bcm4375B1_semco.hcd"   /* UNVERIFIED name */

/* ----------------------------------------------------------------------
 * Device context
 * -------------------------------------------------------------------- */
typedef enum _BT_STATE {
    BtStateInit = 0,      /* resources mapped, UART programmed        */
    BtStateHciUp,         /* R2: ROM answered READ_LOCAL_VERSION      */
    BtStatePatched,       /* R3: patchram + LAUNCH_RAM done           */
    BtStateTransportUp,   /* R4: bthport bound the PDO                */
} BT_STATE;

typedef struct _BT_DEVICE {
    WDFDEVICE   WdfDevice;
    PUCHAR      UartWindow;       /* mapped _CRS descriptor 0 */
    size_t      UartWindowLength;
    WDFINTERRUPT UartInterrupt;   /* GSIV 408  */
    WDFINTERRUPT HostwakeInterrupt; /* GpioInt index 19 */
    BT_STATE    State;
    BOOLEAN     BaudSwitchEnabled; /* registry knob, default OFF (Q2)  */
    ULONG       OperBaud;          /* used only if BaudSwitchEnabled   */
} BT_DEVICE, *PBT_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BT_DEVICE, BtGetDeviceContext)

/* BtTransport.c */
NTSTATUS BtUartInit (_In_ PBT_DEVICE D);
ULONG    BtUartRead (_In_ PBT_DEVICE D, _In_ ULONG Offset);
VOID     BtUartWrite (_In_ PBT_DEVICE D, _In_ ULONG Offset, _In_ ULONG Value);

/* H4 framing (pure logic — unit-testable without hardware) */
NTSTATUS BtH4SendCommand (_In_ PBT_DEVICE D, _In_ USHORT Opcode,
                          _In_ UCHAR ParamLen, _In_reads_(ParamLen) PUCHAR Params);
NTSTATUS BtH4SendAcl    (_In_ PBT_DEVICE D, _In_ PVOID Buffer, _In_ size_t Length);

/* patchram record walker over the .hcd image (pure logic) */
typedef struct _BT_HCD_RECORD { USHORT Opcode; UCHAR ParamLen; PUCHAR Params; } BT_HCD_RECORD;
NTSTATUS BtHcdNextRecord (_Inout_ PUCHAR *Cursor, _In_ PUCHAR End,
                          _Out_ BT_HCD_RECORD *Rec);

#endif /* _EXYNOS9820_BT_H */
