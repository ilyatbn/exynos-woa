# D1 — DWC3 host mode on Exynos 9820: research notes

Plan-of-record task list: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D1.
Findings are appended **as they are discovered**, facts carry source URLs, inference is
marked, dead ends are recorded. Do not rewrite earlier sections.

## Questions the research must answer

1. **Linux/Android bring-up sequence** — in what order does Samsung's 9820 kernel
   (and mainline) initialise the USB host path: CMU gates (PERIC0/FSYS0A), the PHY
   node (`10B00000` USB2 / `10AE0000` combo / `10AF0000` PCS), the `usbfn` glue at
   `10C00000`, and the `dwc3` child (`dr_mode`, VBUS, role)? What registers does each
   step touch?
2. **DWC3 → host mode** — exact register sequence on DWC3 (GCTL/DCTL `PRTCAP_HOST`,
   `dwc3_core_init`, xHCI enable), and anything Exynos-specific around it (suspension,
   refclk, `xhci_l2_support`, `enable_sprs_transfer`).
3. **Mu-Silicium `UsbfnDwc3Dxe`** — what does it actually implement, for which SoCs,
   and why is USB Host still ❌ on Exynos targets? Is it usable as a base for 9820?
4. **edk2-exynos (exynos9820 UEFI port)** — does it have any DWC3/USB host driver at
   all, working or not?
5. **VBUS and the connector** — who drives VBUS 5 V out of the S10e's USB-C port in
   host mode (PMIC? a `vbus` regulator in the DT? the DWC3 core?), and what does a
   **non-powered hub** draw (power budget)?
6. **Windows half (preview for D3)** — how do WoA targets expose a memory-mapped xHCI
   to Windows (ACPI `_HID`/`_CID`, `USBXHCI.sys` binding)? Just a pointer-level answer;
   D3 gets its own pass.

## Progress log

(append below)
