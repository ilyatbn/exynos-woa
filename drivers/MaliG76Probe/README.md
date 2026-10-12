# MaliG76Probe (M16 G9b)

**Origin.** Derived from `drivers/m1probe` of gahingwoo's
[wddm-mali-g52](https://github.com/gahingwoo/wddm-mali-g52) (commit `16f7e7c6`),
GPL-2.0, Copyright gahingwoo. m1probe made the RK3576's Mali-G52 run one
WRITE_VALUE job under Windows; this copy is retargeted to the Galaxy S10e's
Mali-G76 (Exynos 9820). The licence text is `LICENSE` in this directory (the
same file as upstream); this subtree stays GPL-2.0. Credit is also in the
repository's `NOTICE`.

**What changed** (notes/gpu-wddm-mali-g52.md R1-R19, notes/gpu-m16-g9b.md):
binds `ACPI\SAMS9828` (DSDT `GPU0`); checks its resources against the DSDT;
"powered" = PMU+0x2104 and PMU+0x2384 bit0 both 1 (via the PMU page in `_CRS`);
GPU_ID gate for the G76; panfrost's G76 init quirks; one core group only;
the three interrupts are connected and recorded (completion is still polled);
results go to `C:\Windows\System32\LogFiles\MaliG76Probe-run.txt`.
The page-table format, the job descriptor and JS0 are m1probe's, unchanged.

**Not validated on hardware.** Build: `build-package.ps1 -Out <dir>`.
