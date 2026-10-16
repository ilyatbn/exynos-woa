# tools/phone/wddm2 - MaliG76Wddm / WDDM 2.0 debugging kit

Everything used to debug the Mali display/render driver on the S10e from the PC, kept in the repo so a session
reset loses nothing (user request 2026-10-16). Driver: `wddm2dbg.sh` (bash, PC side, SSH host alias `s10e`).
Phone-side scripts go to `C:\S10eTmp` with `wddm2dbg.sh push`. Outputs land in `$WDDM2DBG_OUT` (default `traces/wddm2/` in the repo, gitignored).

| Phone-side file | What it does |
|---|---|
| `probe.ps1 [-Wddm2 3] [-Ver 0] [-WddmVersion 0x2000] [-Quirk 0]` | clears the KMD's registry trace, writes the VOLATILE key `Services\MaliG76Wddm\Parameters\Probe` (gone after any reboot), `pnputil /restart-device ACPI\SAMS9828\0`, prints status + trace. A crash during a probe reboots into the persistent setting. |
| `unprobe.ps1` | deletes the probe key, restarts the device |
| `w2set.ps1 -Wddm2 N [-Reboot]` | persistent `Parameters\Wddm2` (0 = WDDM 1.3 adapter, 3 = WDDM 2.0: WDDMVersion 0x2000 + CalibrateGpuClock/SetStablePowerState stubs); clears WER dumps |
| `w2check.ps1` | adapter status, Wddm2, KMD trace failures (`Fail_*`, `Build`), dwm/explorer + DWM's graphics modules, recent crashes, dumps |
| `kmdswap.cmd` | (SYSTEM task) `C:\S10eTmp\MaliG76Wddm-new.sys` into the DriverStore, old one as `.prev`, hash to `kmdswap.log` |
| `umdswap.cmd` | (SYSTEM task) `C:\S10eTmp\umd\{maliumd,libgallium_d3d10}.dll` into System32, old ones as `.prev` (loaded DLLs can be renamed; the new ones load at the next process start / boot) |
| `espget.cmd` | (SYSTEM task) copies `S:\WINPRAM.BIN` (the ESP is mounted at S: by S10eAgent; `mountvol /s` fails) |
| `umdlog.ps1 [-Exe dwm]` | newest `malikm-<exe>-<pid>.txt` (Mesa winsys `malikm_tlog`, dir = machine env `MALIKM_TGSI_DIR`) |
| `umdtrace.ps1 [-Off]` | machine env `MALIKM_TGSI_DIR=C:\S10eTmp\tlog` + `MALIKM_TRACE=1` (UMD logs per process; DWM after a reboot) |
| `bc.ps1`, `crashq.ps1`, `dwmq.ps1` | Kernel-Power/bugcheck events + dumps; Application Error 1000 (module + offset); DWM process/modules + recent errors |

Facts these rely on (see notes/gpu-wddm2-design.md, notes/gpu-full-adapter-hang.md):
- Scan-out base = DPU DMA G0 `0x19071040` (IDMA base); pitch 4352 after the KMD's re-stride; `tools/phone/fb2png.py`.
- KMD PRAM trace at WINPRAM+0x4000 (0x8FE04000): line 1 = heartbeat `HB i<isr> d<dpc> v<vsync> s<submits>
  p<presents> f<fence>`, lines 2-111 first call of each DDI, 112-255 rolling. S10eDiag ring at +0xA000 holds the
  bugcheck code (`BC 0x...`, `BC P1`, `BC P2`) - our bugcheck callbacks reset the SoC at once, so Windows writes no
  dump and only logs Kernel-Power 41. The firmware saves WINPRAM to the ESP after an unclean reset.
- KMD registry trace: `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9828\0\Device Parameters\MaliG76Wddm`.
- WER LocalDumps for dwm/explorer/d3dtri go to `C:\S10eTmp\dumps`; `wddm2dbg.sh dwmdump NAME` = cdb stack.
- Rescue if the 2.0 adapter breaks the desktop: `wddm2dbg.sh set 0 reboot`; without SSH: TWRP -> ESP
  `\S10EDIAG\nogpu.flg` hides GPU0 (Basic Display).
- Build: `drivers/MaliG76Wddm/build-package.ps1 -Out DIR -Build N` (bump DriverVer in the INF); UMD:
  `tools/mesa-build/build-mesa-g76.ps1 -Stage cross,collect -Out ...\mali-umd-NNNN`, patch exported with
  `git format-patch -1` into tools/mesa-build/patches.
