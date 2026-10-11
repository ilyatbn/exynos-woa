/*
 * S10eBattery.h - KMDF battery miniclass (battc) for the Galaxy S10e (Exynos 9820, beyond0lte).
 *
 * Reports the MAX77705 fuel gauge (7-bit 0x36) and charger status (0x69) on HSI2C45 to the Windows battery
 * class. The driver owns HSI2C45 itself as a polled master, a port of the UEFI code that is proven on the
 * device (firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteUsbHostDxe/Pmic.c + FuelGauge.c).
 * Design and every constant's source: notes/battery-driver-m1.md, notes/battery-driver.md.
 *
 * The WDF/battc plumbing follows Microsoft's Windows-driver-samples "simbatt" sample (MIT License,
 * Copyright (c) Microsoft Corporation).
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>
#include <wmistr.h>
#include <wmilib.h>
#include <ntstrsafe.h>

/* ---------------------------------------------------------------------------------------------------------------
 * MMIO windows (ACPI _CRS of \_SB.BAT0, in this order). VERIFIED values, copied from
 * firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:477-502 - the driver refuses to start unless the
 * resources it is given are exactly these.
 * ------------------------------------------------------------------------------------------------------------- */
#define S10E_HSI2C45_BASE            0x15D60000ULL   /* EXYNOS_PMIC_HSI2C_BASE      (VERIFIED) */
#define S10E_HSI2C45_SIZE            0x1000u         /* EXYNOS_PMIC_HSI2C_SIZE      (VERIFIED) */
#define S10E_CMGP_USI_PAGE_BASE      0x15C22000ULL   /* EXYNOS_CMGP_SYSREG_USI_PAGE (VERIFIED) */
#define S10E_CMGP_USI_PAGE_SIZE      0x1000u         /* ..._USI_PAGE_SIZE (one page; the header does not tag it) */
#define S10E_CMGP_USI03_MODE_OFFSET  0x30u           /* EXYNOS_CMGP_USI03_MODE 0x15C22030 - page base */
#define S10E_USI_V2_MODE_I2C         0x4u            /* EXYNOS_USI_V2_MODE_I2C      (VERIFIED) */
#define S10E_CMGP_GPIO_BASE          0x15C30000ULL   /* EXYNOS_CMGP_GPIO_BASE       (VERIFIED) */
#define S10E_CMGP_GPIO_SIZE          0x1000u         /* EXYNOS_CMGP_GPIO_SIZE       (VERIFIED) */
#define S10E_CMGP_GPM12_CON_OFFSET   0x180u          /* gpm12-0 = SCL               (VERIFIED) */
#define S10E_CMGP_GPM13_CON_OFFSET   0x1A0u          /* gpm13-0 = SDA               (VERIFIED) */
#define S10E_GPIO_FUNC_HSI2C45       0x2u            /* DT samsung,pin-function <2> (VERIFIED) */

/* ---------------------------------------------------------------------------------------------------------------
 * HSI2C register map - same names/values as Beyond0LteUsbHostDxe/UsbHw.h section 5 ([K] = vendor
 * drivers/i2c/busses/i2c-exynos5.c) and the error bits from Pmic.c.
 * ------------------------------------------------------------------------------------------------------------- */
#define HSI2C_CTL                 0x00
#define   HSI2C_CTL_MASTER                  (1u << 3)
#define   HSI2C_CTL_RXCHON                  (1u << 6)
#define   HSI2C_CTL_TXCHON                  (1u << 7)
#define   HSI2C_CTL_SW_RST                  (1u << 31)
#define HSI2C_FIFO_CTL            0x04
#define   HSI2C_FIFO_CTL_RX_EN              (1u << 0)
#define   HSI2C_FIFO_CTL_TX_EN              (1u << 1)
#define   HSI2C_FIFO_CTL_RX_TRIG(x)         ((ULONG)(x) << 4)
#define   HSI2C_FIFO_CTL_TX_TRIG(x)         ((ULONG)(x) << 16)
#define HSI2C_TRAIL_CTL           0x08
#define HSI2C_INT_EN              0x20
#define HSI2C_INT_STAT            0x24
#define   HSI2C_INT_TRANSFER_DONE           (1u << 7)
#define   HSI2C_INT_TRANSFER_ABORT          (1u << 8)
#define   HSI2C_INT_NO_ACK                  (1u << 9)
#define   HSI2C_INT_NODEV                   (1u << 10)
#define   HSI2C_INT_TIMEOUT                 (1u << 11)
#define   HSI2C_INT_ERRORS                  (HSI2C_INT_TRANSFER_ABORT | HSI2C_INT_NO_ACK | HSI2C_INT_NODEV | \
                                             HSI2C_INT_TIMEOUT)
#define   HSI2C_INT_ALL                     0x00000FFFu
#define HSI2C_ERROR_STATUS        0x2C
#define HSI2C_FIFO_STAT           0x30
#define   HSI2C_FIFO_STAT_TX_LEVEL(x)       ((x) & 0x7F)
#define   HSI2C_FIFO_STAT_RX_LEVEL(x)       (((x) >> 16) & 0x7F)
#define HSI2C_TX_DATA             0x34
#define HSI2C_RX_DATA             0x38
#define HSI2C_CONF                0x40
#define   HSI2C_CONF_AUTO_MODE              (1u << 31)
#define HSI2C_AUTO_CONF           0x44
#define   HSI2C_AUTO_CONF_READ_WRITE        (1u << 16)
#define   HSI2C_AUTO_CONF_STOP_AFTER_TRANS  (1u << 17)
#define   HSI2C_AUTO_CONF_MASTER_RUN        (1u << 31)
#define HSI2C_TIMEOUT             0x48
#define HSI2C_TRANS_STATUS        0x50
#define   HSI2C_MASTER_BUSY                 (1u << 17)
#define HSI2C_TIMING_FS1          0x60
#define HSI2C_TIMING_FS2          0x64
#define HSI2C_TIMING_FS3          0x68
#define HSI2C_ADDR                0x70
#define HSI2C_USI_CON             0xC4
/* FS timing: UsbHw.h HSI2C_TIMING_FS*_BOOT = the Windows HSI2C driver's 400 kHz values from the 200 MHz clock,
   device-validated on this bus in UEFI (M16). */
#define HSI2C_TIMING_FS1_BOOT     0x00F00000u
#define HSI2C_TIMING_FS2_BOOT     0x000000E0u
#define HSI2C_TIMING_FS3_BOOT     0x00210000u
#define HSI2C_FIFO_DEPTH          16
#define HSI2C_XFER_TIMEOUT_US     50000u    /* Pmic.c HSI2C_TIMEOUT_MS 50 */

/* ---------------------------------------------------------------------------------------------------------------
 * MAX77705 clients and registers. [K] = vendor source (LineageOS android_kernel_samsung_exynos9820 lineage-18.1:
 * include/linux/mfd/max77705-private.h, drivers/battery_v2/max77705_fuelgauge.c, .../max77705_charger.{c,h}).
 * ------------------------------------------------------------------------------------------------------------- */
#define MAX77705_FG_ADDR          0x36    /* [K] 0x6C >> 1 */
#define MAX77705_FG_STATUS        0x00    /* [K] low byte bit3 = battery absent */
#define   MAX77705_FG_STATUS_ABSENT         0x08
#define MAX77705_FG_REMCAP_REP    0x05    /* [K] */
#define MAX77705_FG_SOCREP        0x06    /* [K] hi byte = %, lo byte = 1/256 % */
#define MAX77705_FG_TEMPERATURE   0x08    /* [K] hi byte signed degC, lo 0.039 degC */
#define MAX77705_FG_VCELL         0x09    /* [K] */
#define MAX77705_FG_AVG_CURRENT   0x0B    /* [K] signed */
#define MAX77705_FG_DESIGNCAP     0x18    /* [K] DESIGNCAP_REG */
#define MAX77705_FG_FULLCAP_REP   0x35    /* [K] */
/* Sense-resistor factor: vendor "standard value is 2 which means 5mOhm". UNVERIFIED for beyond0lte (DT battery
   node not read). Scales mAh/mA only, never the percentage. Indirect support only: UEFI FULLCAP_REP reads of
   2460 (notes/m16-usb.md) and 3000 (task brief, log not committed) at this factor are plausible mAh for a
   3100 mAh-class cell; half or double would not be (notes/battery-driver-m1.md). */
#define MAX77705_FG_RESISTOR_UNVERIFIED     2

#define MAX77705_CHG_ADDR         0x69    /* [K] 0xD2 >> 1 */
/* Charger watchdog (kernel max77705_charger.c set_wdtmr_kick / get_wdtmr_status, max77705_charger.h:124-245):
   CNFG_00 bit4 WDTEN; kick = CNFG_06 bits 1:0 <- 01 (WDTCLR); CHG_DTLS 0x0B = watchdog expired -> charging off.
   Linux kicks it on every health poll; under Windows nobody did, so the phone stopped charging (run batt1). */
#define MAX77705_CHG_CNFG_00      0xB7
#define   MAX77705_MODE_MASK                0xF
#define   MAX77705_MODE_OTG_ON              0xE   /* [K] BUCK+BOOST+OTG, charger off (UsbHw.h:320) */
#define   MAX77705_MODE_CHG_OTG_ON          0xF   /* [K] same + charger engine (UsbHw.h:321) */
#define MAX77705_CHG_CNFG_06      0xBD
#define   MAX77705_CHG_CNFG_06_WDTCLR_MASK  0x03
#define   MAX77705_CHG_CNFG_06_WDTCLR       0x01
#define MAX77705_CHG_DETAILS_00   0xB3    /* [K] bits 6:5 CHGIN_DTLS (3 = VBUS valid) */
#define   MAX77705_CHGIN_DTLS(x)            (((x) & 0x60) >> 5)
#define   MAX77705_CHGIN_DTLS_VALID         3
/* MAX77705_CHG_DETAILS_01 0xB4 [K]: bits 3:0 CHG_DTLS, read as the 2nd byte of the 0xB3 word */
#define   MAX77705_CHG_DTLS(x)              ((x) & 0x0F)

/* Battery nominal voltage for mAh -> mWh (Windows reports energy). UNVERIFIED: close it from the battery label
   (EB-BG970ABU) or the vendor DT battery node. Scales the Wh figures only; the percentage is the gauge's SOCREP. */
#define S10E_BATT_NOMINAL_MV_UNVERIFIED     3850u

/* Plausibility gate, NOT a hardware constant: a single Li-ion cell cannot read outside this window, so a VCELL
   outside it means the transfer returned garbage (e.g. the all-zero 1-byte-read failure mode of this bus). Such a
   reading is dropped instead of being reported - a bogus 0 % would trigger Windows' critical-battery action. */
#define S10E_PLAUSIBLE_VCELL_MIN_MV         2500u
#define S10E_PLAUSIBLE_VCELL_MAX_MV         5000u

#define S10E_BATT_POLL_MS         20000   /* poll period; class queries are answered from the cache */
#define S10E_BATT_TAG             'tBeS'

/* ------------------------------------------------------------------------------------------------------------- */

typedef struct _S10E_BATT_READING {
    ULONG   FgStatus;        /* raw STATUS word */
    ULONG   SocTenths;       /* 0..1000 */
    ULONG   VcellMv;
    LONG    AvgCurrentMa;    /* + = into the battery */
    LONG    TempMilliC;
    ULONG   RemCapMah;
    ULONG   FullCapMah;
    ULONG   DesignCapMah;
    UCHAR   ChgDetails00;
    UCHAR   ChgDetails01;
    BOOLEAN ChargerRead;     /* charger 0x69 answered this poll */
    UCHAR   ChgCnfg00;       /* CHG_CNFG_00 0xB7 (mode, bit4 WDTEN) */
    BOOLEAN Cnfg00Read;      /* ChgCnfg00 is valid */
    UCHAR   ChgCnfg06;       /* CHG_CNFG_06 0xBD before the watchdog kick - diagnostics */
    BOOLEAN WdtKicked;       /* the charger watchdog kick write succeeded this poll */
} S10E_BATT_READING, *PS10E_BATT_READING;

typedef struct _S10E_BATT_GLOBAL {
    UNICODE_STRING RegistryPath;
} S10E_BATT_GLOBAL, *PS10E_BATT_GLOBAL;

typedef struct _S10E_BATT_FDO {
    WDFDEVICE           Device;

    /* MMIO (PrepareHardware / ReleaseHardware) */
    volatile UCHAR     *I2c;
    volatile UCHAR     *UsiPage;
    volatile UCHAR     *GpioPage;

    /* bus (BusLock) */
    WDFWAITLOCK         BusLock;
    BOOLEAN             BusReady;
    BOOLEAN             BusSaved;
    ULONG               SavedMode;
    ULONG               SavedScl;
    ULONG               SavedSda;
    ULONG               SavedUsiCon;

    /* polling */
    WDFTIMER            PollTimer;
    WDFWORKITEM         PollWork;
    ULONG               PollOk;
    ULONG               PollFail;
    NTSTATUS            LastPollStatus;

    /* battery class registration (ClassInitLock) */
    WDFWAITLOCK         ClassInitLock;
    PVOID               ClassHandle;
    WMILIB_CONTEXT      WmiLibContext;

    /* reported state (StateLock) */
    WDFWAITLOCK         StateLock;
    ULONG               BatteryTag;          /* BATTERY_TAG_INVALID until a reading succeeded */
    ULONG               NextTag;
    S10E_BATT_READING   Last;                /* last good reading */
    BATTERY_INFORMATION Info;
    BATTERY_STATUS      Status;
    ULONG               TemperatureDeciK;

    /* unplug diagnosis (StateLock): did the poll see the change, and did the class driver re-query? */
    ULONG               NotifyCount;         /* BatteryClassStatusNotify calls */
    ULONG               QueryStatusCount;    /* QueryStatus calls from the class driver */
    ULONG               QueriedPowerState;   /* PowerState returned by the last QueryStatus */
} S10E_BATT_FDO, *PS10E_BATT_FDO;

#define S10E_BATT_HIST_SLOTS  16             /* Device Parameters Hist00..Hist15, written on every notify */

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S10E_BATT_GLOBAL, S10eBattGetGlobal);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S10E_BATT_FDO, S10eBattGetFdo);

#define S10eBattPrint(Level, Fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (Level), "S10eBattery: " Fmt, __VA_ARGS__)

/* Hsi2c.c - port of Pmic.c (bus setup / reset / polled transfer / release). Caller holds BusLock. */
VOID     S10eHsi2cBusInit(_Inout_ PS10E_BATT_FDO Fdo);
VOID     S10eHsi2cBusRelease(_Inout_ PS10E_BATT_FDO Fdo);
NTSTATUS S10eHsi2cXfer(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR SlaveAddr, _In_ BOOLEAN Read,
                       _Inout_updates_(Length) PUCHAR Data, _In_ ULONG Length, _In_ BOOLEAN Stop);
NTSTATUS S10eMax77705ReadN(_Inout_ PS10E_BATT_FDO Fdo, _In_ UCHAR ClientAddr, _In_ UCHAR Reg,
                           _Out_writes_(Length) PUCHAR Buf, _In_ ULONG Length);

/* Max77705.c - one complete reading. Caller holds BusLock. */
NTSTATUS S10eMax77705Read(_Inout_ PS10E_BATT_FDO Fdo, _Out_ PS10E_BATT_READING Reading);

/* Miniclass.c */
VOID     S10eBattPoll(_In_ WDFDEVICE Device);
VOID     S10eBattInitState(_Inout_ PS10E_BATT_FDO Fdo);
BCLASS_QUERY_TAG_CALLBACK             S10eBattQueryTag;
BCLASS_QUERY_INFORMATION_CALLBACK     S10eBattQueryInformation;
BCLASS_SET_INFORMATION_CALLBACK       S10eBattSetInformation;
BCLASS_QUERY_STATUS_CALLBACK          S10eBattQueryStatus;
BCLASS_SET_STATUS_NOTIFY_CALLBACK     S10eBattSetStatusNotify;
BCLASS_DISABLE_STATUS_NOTIFY_CALLBACK S10eBattDisableStatusNotify;
