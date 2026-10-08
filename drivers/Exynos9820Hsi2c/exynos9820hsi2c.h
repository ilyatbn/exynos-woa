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
// UNVERIFIED — kept at the 9810 values so the package compiles, but they are
// NOT expected to be right on 9820. The 9820 device tree has no node at either
// address; its CMU banks are 1A200000, 1BC00000, 1BD00000, 1BE00000 and
// 1BF00000, and its sysreg controllers are at 13020000 and 13C10000. The
// correct offsets must be derived from Samsung's cal-if headers
// (soc/samsung/cal-if/exynos9820/cmucal-{vclk,node,qch}.h), not guessed.
// ---------------------------------------------------------------------------
//
#define EXYNOS_HSI2C10_PHYSICAL_BASE          0x00000000104B0000ULL   // VERIFIED
#define EXYNOS_HSI2C_REGISTER_LENGTH          0x00001000UL            // VERIFIED

#define EXYNOS_PERIC0_CMU_PHYSICAL_BASE       0x0000000010400000ULL   // UNVERIFIED (9810 value)
#define EXYNOS_PERIC0_CMU_REGISTER_LENGTH     0x00004000UL            // UNVERIFIED (9810 value)
#define EXYNOS_PERIC0_SYSREG_PHYSICAL_BASE    0x0000000010411000ULL   // UNVERIFIED (9810 value)
#define EXYNOS_PERIC0_SYSREG_REGISTER_LENGTH  0x00001000UL            // UNVERIFIED (9810 value)
#define EXYNOS_PERIC0_GPIO_PHYSICAL_BASE      0x0000000010430000ULL   // VERIFIED
#define EXYNOS_PERIC0_GPIO_REGISTER_LENGTH    0x00001000UL            // VERIFIED

#define EXYNOS_HSI2C_SOURCE_CLOCK_HZ          200000000UL             // VERIFIED (default-clk)
#define EXYNOS_HSI2C_DEFAULT_BUS_SPEED_HZ     400000UL
#define EXYNOS_HSI2C_FIFO_TRIGGER             8UL
#define EXYNOS_HSI2C_MAX_TRANSFER_LENGTH      0x00001000UL
#define EXYNOS_HSI2C_REQUEST_TIMEOUT_MS       1000UL

#define EXYNOS_CMU_GATE_USI03_SOURCE          0x2018UL                // UNVERIFIED (9810 offset)
#define EXYNOS_CMU_GATE_USI03_RESET_SYNC      0x206CUL                // UNVERIFIED (9810 offset)
#define EXYNOS_CMU_GATE_USI03_IPCLK           0x20DCUL                // UNVERIFIED (9810 offset)
#define EXYNOS_CMU_GATE_USI03_PCLK            0x20E0UL                // UNVERIFIED (9810 offset)
#define EXYNOS_CMU_QCH_USI03                  0x3038UL                // UNVERIFIED (9810 offset)
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
