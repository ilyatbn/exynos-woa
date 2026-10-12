/** @file
  M16 G3: register offsets read by Beyond0LteGpuProbeDxe. Block bases come from Exynos9820.h (rule 6);
  these are offsets inside them, every one cited (notes/gpu-m16-g2g3.md F3-F5 and "G3 design").

  [SFR] LineageOS lineage-22.2 drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c
  [PMC] same tree, cal-if/exynos9820/flexpmu_cal_local_exynos9820.h (embedded_g3d_save, :569-584)
  [PFR] mainline drivers/gpu/drm/panfrost/panfrost_regs.h (generic Mali JM register map)
  [KID] Arm kbase mali_kbase_gpu_id.h:51-65 (GPU_ID field layout)
**/

#ifndef GPU_PROBE_HW_H_
#define GPU_PROBE_HW_H_

//
// CMU_G3D (EXYNOS_CMU_G3D_BASE). Inside the G3D power domain: [PMC] saves/restores these across power-down.
//
#define CMU_G3D_MUX_EMBEDDED_G3D_USER   0x0100u   // [SFR] PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_USER (MUX_SEL 4, BUSY 7)
#define CMU_G3D_MUX_G3D_BUS_USER        0x0120u   // [SFR] PLL_CON0_MUX_CLKCMU_G3D_BUS_USER
#define CMU_G3D_MUX_G3D_SWITCH_USER     0x0140u   // [SFR] PLL_CON0_MUX_CLKCMU_G3D_SWITCH_USER
#define CMU_G3D_CONTROLLER_OPTION       0x0800u   // [SFR] G3D_CMU_G3D_CONTROLLER_OPTION (29 PM, 28 auto-CG)
#define CMU_G3D_EMB_CONTROLLER_OPTION   0x0804u   // [SFR] G3D_EMBEDDED_CMU_G3D_CONTROLLER_OPTION
#define CMU_G3D_MUX_CLK_G3D_BUSD        0x1000u   // [SFR] CLK_CON_MUX_MUX_CLK_G3D_BUSD
#define CMU_G3D_DIV_CLK_G3D_BUSD        0x1800u   // [SFR] CLK_CON_DIV_DIV_CLK_G3D_BUSD
#define CMU_G3D_DIV_CLK_G3D_BUSP        0x1804u   // [SFR] CLK_CON_DIV_DIV_CLK_G3D_BUSP
#define CMU_G3D_GATE_CMU_PCLK           0x2000u   // [SFR] :1239 CLK_BLK_G3D_UID_G3D_CMU_G3D_IPCLKPORT_PCLK
#define CMU_G3D_GATE_GPU_CLK            0x2004u   // [SFR] :1242 CLK_BLK_G3D_UID_GPU_IPCLKPORT_CLK
#define CMU_GATE_CG_VAL                 (1u << 21)  // [SFR] :6049
#define CMU_GATE_MANUAL                 (1u << 20)  // [SFR] :6050 (cal-if ra.c:415-437: MANUAL=1 -> CG_VAL drives the gate)

//
// PLL_CON0 fields of PLL_G3D (EXYNOS_CMU_TOP_BASE + EXYNOS_CMU_TOP_PLL_G3D_CON0_OFFSET), [SFR] :2761-2765.
//
#define PLL_CON0_ENABLE                 (1u << 31)
#define PLL_CON0_STABLE                 (1u << 29)
#define PLL_CON0_M(V)                   (((V) >> 16) & 0x3FFu)
#define PLL_CON0_P(V)                   (((V) >> 8) & 0x3Fu)
#define PLL_CON0_S(V)                   ((V) & 0x7u)

//
// Mali GPU (EXYNOS_G3D_BASE), read-only ID/status registers, [PFR] line numbers in notes "G3 design".
//
#define GPU_ID                    0x000u
#define GPU_L2_FEATURES           0x004u
#define GPU_CORE_FEATURES         0x008u
#define GPU_TILER_FEATURES        0x00Cu
#define GPU_MEM_FEATURES          0x010u
#define GPU_MMU_FEATURES          0x014u
#define GPU_AS_PRESENT            0x018u
#define GPU_JS_PRESENT            0x01Cu
#define GPU_INT_RAWSTAT           0x020u
#define GPU_STATUS                0x034u
#define GPU_FAULT_STATUS          0x03Cu
#define GPU_THREAD_MAX_THREADS    0x0A0u
#define GPU_THREAD_FEATURES       0x0ACu
#define GPU_SHADER_PRESENT_LO     0x100u
#define GPU_SHADER_PRESENT_HI     0x104u
#define GPU_TILER_PRESENT_LO      0x110u
#define GPU_L2_PRESENT_LO         0x120u
#define GPU_SHADER_READY_LO       0x140u
#define GPU_TILER_READY_LO        0x150u
#define GPU_L2_READY_LO           0x160u
#define GPU_COHERENCY_FEATURES    0x300u
#define GPU_STACK_PRESENT_LO      0xE00u
#define GPU_JOB_INT_RAWSTAT       0x1000u
#define GPU_MMU_INT_RAWSTAT       0x2000u
#define GPU_AS0_STATUS            (0x2400u + 0x28u)   // MMU_AS(0) + AS_STATUS

//
// GPU_ID layout [KID]; G76 match = panfrost GPU_MODEL(g76, 0x7001) (panfrost_gpu.c:226) under the
// 0xF00F product-id mask (panfrost_device.h:236).
//
#define GPU_ID_ARCH_MAJOR(V)      (((V) >> 28) & 0xFu)
#define GPU_ID_ARCH_MINOR(V)      (((V) >> 24) & 0xFu)
#define GPU_ID_ARCH_REV(V)        (((V) >> 20) & 0xFu)
#define GPU_ID_PRODUCT_MAJOR(V)   (((V) >> 16) & 0xFu)
#define GPU_ID_VER_MAJOR(V)       (((V) >> 12) & 0xFu)
#define GPU_ID_VER_MINOR(V)       (((V) >> 4) & 0xFFu)
#define GPU_ID_VER_STATUS(V)      ((V) & 0xFu)
#define GPU_ID_G76_MASK           0xF00Fu
#define GPU_ID_G76_MODEL          0x7001u

#endif // GPU_PROBE_HW_H_
