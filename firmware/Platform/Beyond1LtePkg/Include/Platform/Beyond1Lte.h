/** @file
  Board-specific source facts for the Exynos Galaxy S10 (beyond1lte).

  This header intentionally contains only values sourced from a beyond1lte
  board DTS or a model-specific teardown. It is NOT a complete hardware
  initialisation description. In particular, do not add S10e live-read values
  from Exynos9820.h here.

  Primary source:
    arch/arm64/boot/dts/samsung/exynos9820-beyond1lte_common.dtsi
    arch/arm64/boot/dts/samsung/exynos9820-beyond1lte_input_common.dtsi
    arch/arm64/boot/dts/samsung/exynos9820-beyondlte_input_common.dtsi
    arch/arm64/boot/dts/samsung/exynos9820-evt1-beyond1lte_if-pmic-max77705.dtsi
  Kernel reference: CruelKernel/samsung-exynos9820, cruel-HVJ5-v5.3.
**/

#ifndef BEYOND1LTE_PLATFORM_H
#define BEYOND1LTE_PLATFORM_H

#define BEYOND1LTE_TARGET_CODENAME                 "beyond1lte"
#define BEYOND1LTE_TARGET_SOC                      "Exynos 9820"
#define BEYOND1LTE_TARGET_MODEL_FAMILY             "SM-G973F/DS, SM-G973N (Exynos variants)"

/*
 * Display: S10 board DTS selects s6e3ha9_beyond1. The panel lookup has
 * 1440x3040 L-cut/U-type entries and a 1440x2960 temporary entry. These are
 * source-listed modes, not proof of the panel revision fitted to a given unit.
 */
#define BEYOND1LTE_PANEL_FAMILY                    "s6e3ha9_beyond1"
#define BEYOND1LTE_PANEL_WIDTH                     1440u
#define BEYOND1LTE_PANEL_HEIGHT_PRIMARY            3040u
#define BEYOND1LTE_PANEL_HEIGHT_TEMPORARY          2960u
#define BEYOND1LTE_PANEL_RESET_GPIO                "gpa3-4"
#define BEYOND1LTE_PANEL_RESET_ACTIVE_LOW          1u

/*
 * Touch: BEYOND1/G973, S6SY761 family. Firmware is a stock Samsung firmware
 * name; the binary is not bundled here.
 */
#define BEYOND1LTE_TOUCH_I2C_BUS                   10u
#define BEYOND1LTE_TOUCH_I2C_ADDRESS               0x48u
#define BEYOND1LTE_TOUCH_FIRMWARE_NAME             "tsp_sec/y771_beyond1_phole.bin"
#define BEYOND1LTE_TOUCH_TCLM_LEVEL                2u
#define BEYOND1LTE_TOUCH_AFE_BASE                  0x0206u
#define BEYOND1LTE_TOUCH_IRQ_GPIO                  "gpa1-0"
#define BEYOND1LTE_TOUCH_FOD_SUPPORTED             1u

/*
 * UFS: S10 board override in beyond1lte_common.dtsi.
 * This is the Samsung UFS CAL board selector, not a UFS protocol version.
 */
#define BEYOND1LTE_UFS_CAL_BOARD_TYPE              16u /* UNIV */

/*
 * USB DRD: values from the S10 board DTS. OTG describes the controller's
 * dual-role policy; it does not by itself authorise host-mode VBUS switching.
 */
#define BEYOND1LTE_USB_DR_MODE                     "otg"
#define BEYOND1LTE_USB_MAXIMUM_SPEED               "super-speed-plus"
#define BEYOND1LTE_USB_PHY_PORT_GPIO               "gpg0-4"

/*
 * USB-C / PD: from the EVT1-specific S10 MAX77705 overlay. Verify that the
 * target's actual DTBO/revision uses this overlay before using the IRQ.
 */
#define BEYOND1LTE_MAX77705_I2C_BUS                45u
#define BEYOND1LTE_MAX77705_I2C_ADDRESS            0x66u
#define BEYOND1LTE_MAX77705_IRQ_GPIO               "gpa1-6"
#define BEYOND1LTE_MAX77705_SUPPORTS_ROLE_SWAP     1u

/*
 * Physical buttons: shared beyond-family input DTS. Linux key codes are
 * retained as source identifiers; a UEFI driver must map them deliberately.
 */
#define BEYOND1LTE_KEY_WINK_GPIO                   "gpa0-6"
#define BEYOND1LTE_KEY_WINK_LINUX_CODE             703u
#define BEYOND1LTE_KEY_VOLUMEDOWN_GPIO              "gpa0-4"
#define BEYOND1LTE_KEY_VOLUMEDOWN_LINUX_CODE        114u
#define BEYOND1LTE_KEY_VOLUMEUP_GPIO                "gpa0-3"
#define BEYOND1LTE_KEY_VOLUMEUP_LINUX_CODE          115u
#define BEYOND1LTE_KEY_POWER_GPIO                   "gpa2-4"
#define BEYOND1LTE_KEY_POWER_LINUX_CODE             116u
#define BEYOND1LTE_KEY_DEBOUNCE_MS                  10u

/*
 * Fail-closed readiness markers. Keep these at zero until the matching
 * board-specific handoff data has been sourced and the implementation tested.
 * These do not yet gate a build; the first Beyond1LtePkg DSC must add explicit
 * compile-time guards before any of these subsystems is enabled.
 */
#define BEYOND1LTE_MEMORY_MAP_READY                 0u
#define BEYOND1LTE_UFS_DMA_WINDOW_READY              0u
#define BEYOND1LTE_FRAMEBUFFER_HANDOFF_READY        0u
#define BEYOND1LTE_USB_C_ROLE_POWER_READY            0u
#define BEYOND1LTE_BUTTON_GPIO_ELECTRICAL_READY      0u

#endif /* BEYOND1LTE_PLATFORM_H */
