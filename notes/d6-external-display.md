# D6 — External display (HDMI via the dock): research notes

Plan-of-record task list: [`docs/exynos9820-port/DRIVERS-MILESTONES.md`](../docs/exynos9820-port/DRIVERS-MILESTONES.md) §D6.
Findings appended as discovered; facts carry source URLs; inference marked; dead
ends recorded.

## Known already (do not re-derive)

- The user's hub is **non-powered and HAS an HDMI port** (2026-10-10) — model
  not yet identified. Two completely different mechanisms hide behind "USB-C
  hub with HDMI": **DP alt mode** (the HDMI socket is wired to the phone's
  SuperSpeed lanes through the combo PHY — no USB data involved for video) vs
  **DisplayLink** (HDMI rendered by a chip in the dock, fed over USB data).
  Identification matters more than any driver work.
- `notes/dock-desktop.md` §11: the 9820 DT has a **USB/DP combo PHY**
  (`10AE0000`+0x2800, `has_combo_phy=1`) and a `displayport@130B0000` node;
  DeX on the S10e is listed as DP-alt-mode capable.
- D1 research found the PHY_CON GPIO (`gpg0 4`) muxes the combo PHY between
  USB and DP paths, read by the kernel PHY driver at init.
- `Exynos9820.h` already carries **DECON_F** (phone panel) and **DECON_T**
  windows — decon_t is the second display pipeline (the "T" is for TV/external
  in Exynos naming — verify).
- M11 established: DisplayLink ships a **Windows ARM64 WDDM driver** (Synaptics
  11.3+), and DisplayLink's own docs say the phone must become the USB host —
  which is exactly what D1 built.
- D5/D5b established: Windows enumerates displays ONLY through WDDM drivers;
  our shipped display path is GOP + in-box BasicDisplay (one panel, no
  second-output concept).

## Questions the research must answer

1. **Identify the two hub types from the outside**: what in a product's spec
   sheet / chip marks / behaviour says DP-alt-mode vs DisplayLink vs MHL?
   Cheap "USB-C hub with HDMI + USB2 ports" — which is it usually?
2. **DP alt mode on the 9820 in the Samsung kernel**: which driver binds
   `displayport@130B0000` (exynos DP? "smv" stack?), what is the full
   enable sequence (combo PHY lane flip/swap, HPD, the gpg0-4 PHY_CON GPIO,
   DECON_T -> DP link training), and what does DeX actually drive through it?
3. **DECON_T facts**: is decon_t really the external pipeline on this SoC
   (DT status, clocks, interrupts in Exynos9820.h vs the vendor DTS)?
4. **UEFI feasibility**: could our firmware light an external HDMI at all
   (DECON_T + DP init in DXE), and does any prior art exist (edk2-exynos,
   mainline DRM as documentation)?
5. **Windows feasibility per path**: (a) DP alt mode needs a WDDM driver for
   DECON_T+DP (ties to D5b rung 1 — display-only WDDM); (b) DisplayLink needs
   NOTHING from us beyond working USB host (D1) + the vendor ARM64 driver —
   confirm current driver + whether it is WHQL-signed (installable without
   test-signing) for a donation build.
6. **Verdict**: given the user's hub has HDMI, which mechanism is it likely to
   be, what is the cheapest validation on our device, and what does each path
   cost.

## Progress log

(append below)
