# D5 — GPU / display on the Exynos 9820: research notes

Plan-of-record task list: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D5.
Findings appended as discovered; facts carry source URLs; inference marked; dead
ends recorded.

## What this repo already has (do not re-research)

- `SimpleFramebufferDxe` in Beyond0LtePkg: GOP over the sboot-lit framebuffer at
  `EXYNOS_FB_BASE 0xCA000000` (1440x3040) — UEFI already draws.
- The boot shim paints DECON directly (`0x1281`->`decon_f 0x19030000` enable, from
  edk2-exynos `PlatformUtils.c`) — prior art for DECON register access.
- Combo PHY facts (`has_combo_phy=1`, `displayport@130B0000`) in
  `notes/dock-desktop.md` §11 — D6 (external display) owns the DP-alt-mode half.
- MILESTONES M8 open task: "Fix the framebuffer RAM/DEV overlap" (`EXYNOS_FB_BASE`
  published MMAP_DEV inside a bank0 RAM gap) — interacts with any fb work.

## Questions the research must answer

1. **Mali-G76 (Bifrost, 11-core G76?) on Windows ARM64** — does ANY WDDM driver
   exist or has anyone run a Mali GPU under Windows on ARM (Mu-Silicium status,
   edk2-exynos, WoA community)? ARM's own Windows Mali driver availability
   (NDA/partner-only? the "Mali Display Processor driver for Windows" is DP, not
   GPU — confirm the distinction).
2. **What do the Exynos Linux ports use for GPU** — panfrost vs lima vs Samsung
   blob for the 9820's Mali-G76; what kernel/driver does
   `chiffathefox/exynos-9820-mainline-linux` / postmarketOS ship; is the G76
   Bifrost generation panfrost-supported and to what degree (features/bugs)?
3. **What does Mu-Silicium do for display under Windows on Samsung targets** —
   SimpleFbDxe-style framebuffer only? Any DECON/DSI driver work? What does
   Status.md say for Display on beyond1lte/starlte?
4. **Windows basic display on a framebuffer** — what is the realistic Windows
   half: a WDDM "Indirect Display" or basic fb driver? What do Snapdragon WoA
   ports without GPU drivers show on screen (Mu on Lumia 950 = ?)? Is there a
   maintained simplefb->WDDM shim (e.g. the "BasicRender"/framebuffer WDDM
   samples)?
5. **Verdict inputs** — cost/benefit: is any GPU acceleration reachable on this
   SoC under Windows at all, or is the honest deliverable "2D framebuffer
   display, no acceleration"?

## Progress log

(append below)
