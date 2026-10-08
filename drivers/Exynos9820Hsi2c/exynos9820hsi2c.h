#ifndef _EXYNOS9820_HSI2C_H_
#define _EXYNOS9820_HSI2C_H_

//
// ---------------------------------------------------------------------------
// Exynos 9820 (Galaxy S10e, `beyond0lte`) HSI2C10 register blocks.
//
// VERIFIED against the shipping 9820 device tree and a live S10e — see
// notes/dock-desktop.md §12 in this repo:
//
//   * the bus window      hsi2c@104B0000, size 0x1000      IDENTICAL to 9810
//   * the source clock    default-clk = 0xbebc200 = 200 MHz IDENTICAL to 9810
//   * the GPIO window     pinctrl@10430000                  IDENTICAL to 9810
//   * the I2C pin pair    gpio_scl/gpio_sda = gpp1 pins 4/5 IDENTICAL to 9810
//   * the EHI2C register set and the USI03 clock NAMES     IDENTICAL to 9810
//
// RESOLVED 2026-10-09 from Samsung's own calibration table, not from the DT:
// `drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c` opens with an
// `SFR_BLOCK(name, address, size)` list of ALL 34 CMU blocks, and the DT's
// five-window `cmu` node is only a subset of it (BUSC + MIF..MIF3). The two
// blocks this driver needs are in that list:
//
//   SFR_BLOCK(CMU_PERIC0, 0x10400000, 0x8000)   -> same BASE as the 9810,
//   SFR_BLOCK(CMU_FSYS0A, 0x10a00000, 0x8000)      but the full block is 0x8000
//
// so the 9810's CMU base was right and only its LENGTH (0x4000, the cal-if
// `dbg_offset`) was short. The gate offsets below are the SFR entries whose
// NAMES match the roles the driver uses; the 9810's numbers were a different
// block layout, so they do not carry over:
//
//   CLK_CON_GAT_CLK_BLK_PERIC0_UID_RSTNSYNC_CLK_PERIC0_OSCCLK_IPCLKPORT_CLK  0x2008
//   CLK_CON_GAT_CLK_BLK_PERIC0_UID_RSTNSYNC_CLK_PERIC0_USI_I2C_IPCLKPORT_CLK 0x2054
//   CLK_CON_GAT_GOUT_BLK_PERIC0_UID_USI03_I2C_IPCLKPORT_IPCLK                0x2094
//   CLK_CON_GAT_GOUT_BLK_PERIC0_UID_USI03_I2C_IPCLKPORT_PCLK                 0x2098
//   QCH_CON_USI03_I2C_QCH                                                    0x3038 (identical to 9810)
//
// The bit fields agree with the driver's masks: SFR_ACCESS(..._MANUAL, 20, 1)
// and SFR_ACCESS(..._CG_VAL, 21, 1) match EXYNOS_CMU_GATE_MANUAL (1<<20) and
// EXYNOS_CMU_GATE_VALUE (1<<21).
//
// STILL UNVERIFIED: the SYSREG window. The 9820 DT has no node at 0x10411000;
// its USI sysreg block is `usi@10421000` with `usi3` at 0x10421010, which is
// the candidate (base 0x10421000, USI03 register at +0x10) — but the 9810's
// +0x1C slot does not translate, so it stays at the 9810 value and the DSDT
// keeps the pair in agreement.
// ---------------------------------------------------------------------------
//
#define EXYNOS_HSI2C10_PHYSICAL_BASE          0x00000000104B0000ULL   // VERIFIED
#define EXYNOS_HSI2C_REGISTER_LENGTH          0x00001000UL            // VERIFIED

#define EXYNOS_PERIC0_CMU_PHYSICAL_BASE       0x0000000010400000ULL   // cal-if SFR_BLOCK
#define EXYNOS_PERIC0_CMU_REGISTER_LENGTH     0x00008000UL            // cal-if SFR_BLOCK size
#define EXYNOS_PERIC0_SYSREG_PHYSICAL_BASE    0x0000000010411000ULL   // UNVERIFIED (9810 value; 0x10421000 is the candidate)
#define EXYNOS_PERIC0_SYSREG_REGISTER_LENGTH  0x00001000UL            // UNVERIFIED (9810 value)
#define EXYNOS_PERIC0_GPIO_PHYSICAL_BASE      0x0000000010430000ULL   // VERIFIED
#define EXYNOS_PERIC0_GPIO_REGISTER_LENGTH    0x00001000UL            // VERIFIED

#define EXYNOS_HSI2C_SOURCE_CLOCK_HZ          200000000UL             // VERIFIED (default-clk)
#define EXYNOS_HSI2C_DEFAULT_BUS_SPEED_HZ     400000UL
#define EXYNOS_HSI2C_FIFO_TRIGGER             8UL
#define EXYNOS_HSI2C_MAX_TRANSFER_LENGTH      0x00001000UL
#define EXYNOS_HSI2C_REQUEST_TIMEOUT_MS       1000UL

#define EXYNOS_CMU_GATE_USI03_SOURCE          0x2008UL                // cal-if: RSTNSYNC_CLK_PERIC0_OSCCLK_IPCLKPORT_CLK (chain-inferred role)
#define EXYNOS_CMU_GATE_USI03_RESET_SYNC      0x2054UL                // cal-if: RSTNSYNC_CLK_PERIC0_USI_I2C_IPCLKPORT_CLK
#define EXYNOS_CMU_GATE_USI03_IPCLK           0x2094UL                // cal-if: USI03_I2C_IPCLKPORT_IPCLK
#define EXYNOS_CMU_GATE_USI03_PCLK            0x2098UL                // cal-if: USI03_I2C_IPCLKPORT_PCLK
#define EXYNOS_CMU_QCH_USI03                  0x3038UL                // cal-if: QCH_CON_USI03_I2C_QCH (same as 9810)
#define EXYNOS_CMU_GATE_MANUAL                (1UL << 20)
#define EXYNOS_CMU_GATE_VALUE                 (1UL << 21)
#define EXYNOS_CMU_QCH_ENABLE                 (1UL << 0)
#define EXYNOS_CMU_QCH_CLOCK_REQUEST          (1UL << 1)
#define EXYNOS_CMU_QCH_REQUIRED               \
    (EXYNOS_CMU_QCH_ENABLE | EXYNOS_CMU_QCH_CLOCK_REQUEST)

#define EXYNOS_SYSREG_USI03_CONFIGURATION     0x001CUL
#define EXYNOS_SYSREG_USI_MODE_I2C            0x00000004UL

#define EXYNOS_GPIO_GPP1_CONFIGURATION        0x0020UL
#define EXYNOS_GPIO_GPP1_DATA                 0x0024UL
#define EXYNOS_GPIO_GPP1_PULL                 0x0028UL
#define EXYNOS_GPIO_GPP1_DRIVE                0x002CUL
#define EXYNOS_GPIO_GPP1_SCL_PIN              4UL
#define EXYNOS_GPIO_GPP1_SDA_PIN              5UL
#define EXYNOS_GPIO_GPP1_SCL_SHIFT            16UL
#define EXYNOS_GPIO_GPP1_SDA_SHIFT            20UL
#define EXYNOS_GPIO_PIN_FIELD_MASK            0xFUL
#define EXYNOS_GPIO_PIN_FUNCTION_I2C           0x2UL

#define EXYNOS_USI_CONTROL                    0x00C4UL
#define EXYNOS_USI_OPTION                     0x00C8UL
#define EXYNOS_USI_CONTROL_RESET              (1UL << 0)

#define EHI2C_CONTROL                         0x0000UL
#define EHI2C_FIFO_CONTROL                    0x0004UL
#define EHI2C_TRAILING_CONTROL                0x0008UL
#define EHI2C_INTERRUPT_ENABLE                0x0020UL
#define EHI2C_INTERRUPT_STATUS                0x0024UL
#define EHI2C_ERROR_STATUS                    0x002CUL
#define EHI2C_FIFO_STATUS                     0x0030UL
#define EHI2C_TX_DATA                         0x0034UL
#define EHI2C_RX_DATA                         0x0038UL
#define EHI2C_CONFIGURATION                   0x0040UL
#define EHI2C_AUTO_CONFIGURATION              0x0044UL
#define EHI2C_TIMEOUT                         0x0048UL
#define EHI2C_TRANSFER_STATUS                 0x0050UL
#define EHI2C_TIMING_FS1                      0x0060UL
#define EHI2C_TIMING_FS2                      0x0064UL
#define EHI2C_TIMING_FS3                      0x0068UL
#define EHI2C_TIMING_SLA                      0x006CUL
#define EHI2C_ADDRESS                         0x0070UL

#define EHI2C_CONTROL_MASTER                  (1UL << 3)
#define EHI2C_CONTROL_RX_CHANNEL              (1UL << 6)
#define EHI2C_CONTROL_TX_CHANNEL              (1UL << 7)
#define EHI2C_CONTROL_SOFTWARE_RESET          (1UL << 31)

#define EHI2C_FIFO_RX_ENABLE                  (1UL << 0)
#define EHI2C_FIFO_TX_ENABLE                  (1UL << 1)
#define EHI2C_FIFO_RX_TRIGGER(_Level)         ((_Level) << 4)
#define EHI2C_FIFO_TX_TRIGGER(_Level)         ((_Level) << 16)

#define EHI2C_INTERRUPT_TX_ALMOST_EMPTY       (1UL << 0)
#define EHI2C_INTERRUPT_RX_ALMOST_FULL        (1UL << 1)
#define EHI2C_INTERRUPT_TRAILING              (1UL << 6)
#define EHI2C_INTERRUPT_TRANSFER_DONE         (1UL << 7)
#define EHI2C_INTERRUPT_TRANSFER_ABORT        (1UL << 8)
#define EHI2C_INTERRUPT_NO_ACK                (1UL << 9)
#define EHI2C_INTERRUPT_NO_DEVICE             (1UL << 10)
#define EHI2C_INTERRUPT_TIMEOUT               (1UL << 11)
#define EHI2C_INTERRUPT_ERROR_MASK            \
    (EHI2C_INTERRUPT_TRANSFER_ABORT | EHI2C_INTERRUPT_NO_ACK | \
     EHI2C_INTERRUPT_NO_DEVICE | EHI2C_INTERRUPT_TIMEOUT)
#define EHI2C_INTERRUPT_COMPLETION_MASK       \
    (EHI2C_INTERRUPT_TRANSFER_DONE | EHI2C_INTERRUPT_ERROR_MASK)
#define EHI2C_INTERRUPT_ALL                   0x00000FFFUL

#define EHI2C_FIFO_TX_FULL                    (1UL << 7)
#define EHI2C_FIFO_TX_EMPTY                   (1UL << 8)
#define EHI2C_FIFO_RX_FULL                    (1UL << 23)
#define EHI2C_FIFO_RX_EMPTY                   (1UL << 24)
#define EHI2C_FIFO_TX_LEVEL(_Value)           ((_Value) & 0x7FUL)
#define EHI2C_FIFO_RX_LEVEL(_Value)           (((_Value) >> 16) & 0x7FUL)

#define EHI2C_CONFIGURATION_AUTO_MODE         (1UL << 31)
#define EHI2C_CONFIGURATION_10_BIT_ADDRESS    (1UL << 30)

#define EHI2C_AUTO_READ                       (1UL << 16)
#define EHI2C_AUTO_STOP                       (1UL << 17)
#define EHI2C_AUTO_RUN                        (1UL << 31)
#define EHI2C_AUTO_LENGTH_MASK                0x0000FFFFUL

#define EHI2C_TIMEOUT_ENABLE                  (1UL << 31)

#define EHI2C_TRANSFER_MASTER_BUSY            (1UL << 17)
#define EHI2C_TRANSFER_SLAVE_BUSY             (1UL << 16)
#define EHI2C_TRANSFER_MASTER_STATE_MASK      0xFUL
#define EHI2C_TRANSFER_MASTER_IDLE            0x0UL
#define EHI2C_TRANSFER_MASTER_START           0x1UL
#define EHI2C_TRANSFER_MASTER_RESTART         0x2UL
#define EHI2C_TRANSFER_MASTER_STOP            0x3UL
#define EHI2C_TRANSFER_MASTER_ID              0x4UL
#define EHI2C_TRANSFER_MASTER_ADDRESS_0       0x5UL
#define EHI2C_TRANSFER_MASTER_ADDRESS_1       0x6UL
#define EHI2C_TRANSFER_MASTER_ADDRESS_2       0x7UL
#define EHI2C_TRANSFER_MASTER_ADDRESS_SR      0x8UL
#define EHI2C_TRANSFER_MASTER_READ            0x9UL
#define EHI2C_TRANSFER_MASTER_WRITE           0xAUL
#define EHI2C_TRANSFER_MASTER_NO_ACK          0xBUL
#define EHI2C_TRANSFER_MASTER_LOST            0xCUL
#define EHI2C_TRANSFER_MASTER_WAIT            0xDUL
#define EHI2C_TRANSFER_MASTER_WAIT_COMMAND    0xEUL

#define EHI2C_ADDRESS_MASTER(_Address)        (((_Address) & 0x3FFUL) << 10)
#define EHI2C_TRAILING_COUNT_MAX              0x00FFFFFFUL

FORCEINLINE
BOOLEAN
ExynosTestAnyBits(
    _In_ ULONG Value,
    _In_ ULONG Mask
    )
{
    return (Value & Mask) != 0;
}

#endif
