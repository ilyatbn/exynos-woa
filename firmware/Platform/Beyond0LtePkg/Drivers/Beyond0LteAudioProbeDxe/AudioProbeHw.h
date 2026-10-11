// AudioProbeHw.h - controller register maps the D8a audio probe drives.
//
// [S] = drivers/spi/spi-s3c64xx.c, [I] = drivers/i2c/busses/i2c-exynos5.c, both LineageOS
// android_kernel_samsung_exynos9820 @ lineage-22.2 (notes/d8-audio-m1.md §1.2). The HSI2C set is
// the same one Beyond0LteUsbHostDxe/UsbHw.h uses for hsi2c_45 (device-validated on the MAX77705 bus,
// M16/M17); copied here so the 9820 USB driver stays untouched.

#ifndef AUDIO_PROBE_HW_H_
#define AUDIO_PROBE_HW_H_

//
// Exynos USI SPI (samsung,exynos-spi, exynos_spi_port_config: clk_from_cmu, rx_lvl_offset 15).
//
#define SPI_CH_CFG                0x00    // [S:50]
#define   SPI_CH_HS_EN                    BIT6    // [S:63]
#define   SPI_CH_SW_RST                   BIT5    // [S:64]
#define   SPI_CH_SLAVE                    BIT4    // [S:65]
#define   SPI_CPOL_L                      BIT3    // [S:66]
#define   SPI_CPHA_B                      BIT2    // [S:67]
#define   SPI_CH_RXCH_ON                  BIT1    // [S:68]
#define   SPI_CH_TXCH_ON                  BIT0    // [S:69]
#define SPI_MODE_CFG              0x08    // [S:52]
#define   SPI_MODE_CH_TSZ_MASK            (3u << 29)  // [S:79] 0 = byte
#define   SPI_MODE_BUS_TSZ_MASK           (3u << 17)  // [S:83] 0 = byte
#define   SPI_MODE_TRAILCNT               (0x3FFu << 19)  // [S:136-139] MAX_TRAILCNT << TRAILCNT_OFF
#define   SPI_MODE_RXDMA_ON               BIT2    // [S:85]
#define   SPI_MODE_TXDMA_ON               BIT1    // [S:86]
#define   SPI_MODE_4BURST                 BIT0    // [S:87]
#define SPI_SLAVE_SEL             0x0C    // [S:53]
#define   SPI_SLAVE_SIG_INACT             BIT0    // [S:92] manual CS: 1 = deasserted, 0 = asserted
#define SPI_INT_EN                0x10    // [S:54]
#define SPI_STATUS                0x14    // [S:55]
#define   SPI_STATUS_TX_LVL(s)            (((s) >> 6) & 0x7F)   // [S:132] fifo_lvl_mask (64<<1)-1 [S:1780]
#define   SPI_STATUS_RX_LVL(s)            (((s) >> 15) & 0x7F)  // [S:133] rx_lvl_offset 15 [S:2195]
#define SPI_TX_DATA               0x18    // [S:56]
#define SPI_RX_DATA               0x1C    // [S:57]
#define SPI_PACKET_CNT            0x20    // [S:58]
#define   SPI_PACKET_CNT_EN               BIT16   // [S:109]
#define SPI_PENDING_CLR           0x24    // [S:59]
#define   SPI_PND_ALL                     0x1Eu   // [S:112-115, 1462-1465] TX/RX under/overrun
#define SPI_SWAP_CFG              0x28    // [S:60]
#define SPI_FB_CLK                0x2C    // [S:61] feedback delay, DT spi-feedback-delay = 1
#define SPI_USI_CON               0xC4    // [S:152] write 0 = USI reset released [S:1430]
#define SPI_FIFO_DEPTH            64u     // DT samsung,spi-fifosize

//
// Exynos HSI2C (samsung,exynos5-hsi2c), polled master. [I] + UsbHw.h.
//
#define HSI2C_CTL                 0x00
#define   HSI2C_CTL_RXCHON                BIT6
#define   HSI2C_CTL_TXCHON                BIT7
#define   HSI2C_CTL_MASTER                BIT3
#define   HSI2C_CTL_SW_RST                BIT31
#define HSI2C_FIFO_CTL            0x04
#define   HSI2C_FIFO_CTL_RX_EN            BIT0
#define   HSI2C_FIFO_CTL_TX_EN            BIT1
#define   HSI2C_FIFO_CTL_RX_TRIG(x)       (((UINT32)(x) & 0x7F) << 4)
#define   HSI2C_FIFO_CTL_TX_TRIG(x)       (((UINT32)(x) & 0x7F) << 16)
#define HSI2C_TRAIL_CTL           0x08
#define HSI2C_INT_EN              0x20
#define HSI2C_INT_STAT            0x24
#define   HSI2C_INT_TRANSFER_DONE         BIT7
#define   HSI2C_INT_TRANSFER_ABORT        BIT8
#define   HSI2C_INT_NO_ACK                BIT9
#define   HSI2C_INT_NODEV                 BIT10
#define   HSI2C_INT_TIMEOUT               BIT11
#define   HSI2C_INT_ERRORS                (HSI2C_INT_TRANSFER_ABORT | HSI2C_INT_NO_ACK | HSI2C_INT_NODEV | HSI2C_INT_TIMEOUT)
#define   HSI2C_INT_ALL                   0x00000FFFu
#define HSI2C_ERROR_STATUS        0x2C
#define HSI2C_FIFO_STAT           0x30
#define   HSI2C_FIFO_STAT_TX_LEVEL(x)     ((x) & 0x7F)
#define   HSI2C_FIFO_STAT_RX_LEVEL(x)     (((x) >> 16) & 0x7F)
#define HSI2C_TX_DATA             0x34
#define HSI2C_RX_DATA             0x38
#define HSI2C_CONF                0x40
#define   HSI2C_CONF_AUTO_MODE            BIT31
#define HSI2C_AUTO_CONF           0x44
#define   HSI2C_AUTO_CONF_READ_WRITE      BIT16
#define   HSI2C_AUTO_CONF_STOP_AFTER_TRANS BIT17
#define   HSI2C_AUTO_CONF_MASTER_RUN      BIT31
#define HSI2C_TIMEOUT             0x48
#define HSI2C_TRANS_STATUS        0x50
#define   HSI2C_MASTER_BUSY               BIT17
#define HSI2C_TIMING_FS1          0x60
#define HSI2C_TIMING_FS2          0x64
#define HSI2C_TIMING_FS3          0x68
#define HSI2C_ADDR                0x70
#define HSI2C_USI_CON             0xC4
// 400 kHz from the DT default-clk 200 MHz (UsbHw.h HSI2C_TIMING_FS*_BOOT, same formula as the
// device-validated Windows HSI2C driver). The real hsi2c_38 ipclk (DIV_CLK_PERIC1_USI_I2C) is
// UNVERIFIED - it is logged from CMU_PERIC1; a slower ipclk only means a slower SCL.
#define HSI2C_TIMING_FS1_400K     0x00F00000
#define HSI2C_TIMING_FS2_400K     0x000000E0
#define HSI2C_TIMING_FS3_400K     0x00210000

//
// CMU gate CON bits [CAL cmucal-sfr.c SFR_ACCESS(..._CG_VAL, 21, 1) / (_MANUAL, 20, 1)].
//
#define CMU_GATE_CG_VAL           BIT21
#define CMU_GATE_MANUAL           BIT20

//
// Chip registers.
//
#define CS47L93_REG_SOFTWARE_RESET   0x0000u   // reads the silicon ID (madera-core.c:780)
#define CS47L93_REG_HW_REVISION      0x0001u   // & 0xFF (registers.h:18, MADERA_HW_REVISION_MASK)
#define CS47L93_REG_IRQ1_RAW_STATUS1 0x1880u   // BOOT_DONE_STS1 = bit 7 (registers.h:1505, 7932)
#define CS47L93_BOOT_DONE            BIT7
#define CS47L93_SPI_READ             0x80u     // regmap-spi read_flag_mask, top byte of the 32-bit address

#define CS35L40_REG_DEVID            0x00000000u   // cs35l41 registers.h:22
#define CS35L40_REG_REVID            0x00000004u   // registers.h:23
#define CS35L40_REG_OTPID            0x00000010u   // registers.h:26
#define CS35L40_REG_IRQ1_STATUS4     0x0001001Cu   // registers.h:121, OTP_BOOT_DONE = bit 1
#define CS35L40_OTP_BOOT_DONE        BIT1

#endif
