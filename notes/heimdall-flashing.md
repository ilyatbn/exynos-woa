# heimdall — the CLI path to flash the S10e (Odin's open-source equivalent)

Started: 2026-10-10. Research task, no device writes. Phone is being restored to
stock Android by the user; nothing was flashed from here.

## Progress log

### H01 — the tool and its versions

- Canonical repo: https://github.com/Benjamin-Dobell/Heimdall (MIT). README says
  it speaks "the custom Samsung-developed protocol typically referred to as the
  'Odin 3 protocol'" to the device's download-mode stub ("Loke"), over libusb.
  Source: https://raw.githubusercontent.com/Benjamin-Dobell/Heimdall/master/README.md
- Upstream's last release is **1.4.x**, but the repo's own issue thread shows a
  **v2.1.0** build in use: "Heimdall v2.1.0 … Copyright (c) 2010-2017 Benjamin
  Dobell … Copyright (c) 2021-2024 **Henrik Grimler**" — i.e. the maintained
  fork is **Grimler's** (https://git.sr.ht/~grimler/Heimdall, mirrored on GitHub;
  the `amo13/Heimdall` fork is "the original repository with a few crucial pull
  requests merged"). Source: https://github.com/Benjamin-Dobell/Heimdall/issues/547
- **FACT: v2.1.0 works on modern Samsungs.** Issue #547 is a 2021-era device
  (`A52Q_EUR_OPEN.pit`) where the run got to "Session begun" and uploaded every
  partition; the failure there was only `--repartition` ("Failed to confirm end
  of PIT file transfer"), and the reporter says "Removing `--repartition` works".
  Source: same issue.
- **Counter-evidence (newer device):** issue #548 on `SM-A146U` (A14 5G, 2025)
  fails at "Uploading boot … ERROR: Failed to confirm transfer initialisation!".
  Source: https://github.com/Benjamin-Dobell/Heimdall/issues/548
- **INFERENCE (mine):** the S10e (2019, Exynos 9820) sits between those two data
  points. Untested here. The cheap test is `heimdall detect` and
  `heimdall print-pit` — both **read-only** (they download the device's PIT,
  they write nothing).

### H02 — the CLI shape we need

From the user guide (https://github.com/jeffjose/heimdall-userguide) and the
Win32 README:

1. `adb reboot download` (or long-press VolDown+Power+Home) → download mode.
2. `heimdall detect` → "Device detected".
3. `heimdall print-pit` → the partition names and sizes **as the device
   declares them**. The guide's sample shows entries named `BOOT`
   (`Partition Block Count: 20480`) and `RECOVERY` (`20480`), with flash
   filenames `boot.img` / `recovery.img`.
4. `heimdall flash --RECOVERY <file> --no-reboot` — one partition, no
   repartition (repartitioning **wipes the device**, per the Win32 README).

### H03 — the two hazards this tool has, both relevant to our image

- **No size check.** Open PR #555 states the defect plainly: "Heimdall currently
  allows flashing files larger than the target partition with no warning. On
  some devices this silently overwrites adjacent partitions."
  Source: https://github.com/Benjamin-Dobell/Heimdall/pull/555
  Our candidate is **18 255 872 B** into a recovery slot the repo measured at
  **55 279 616 B** (`notes/s10e-partitions.md`, `notes/K10-rkp.md` T18) — a
  3.0× margin, so we are on the safe side of that defect, but the check must be
  done by hand against `print-pit` output, not assumed.
- **Block-size ambiguity.** The same PR notes the PIT's
  `blockSizeOrOffset` field is ambiguous (offset or size depending on the Loke
  version), so `print-pit` numbers must be cross-checked against the GPT reads
  already in `notes/s10e-root-reads.md` §2/§8 before any flash.

### H04 — dead ends

- grep.app `q="--dingo"` → 2 hits, both unrelated (glibc test, RabbitMQ
  autocomplete). There is **no** `--dingo` flag in heimdall; that name came from
  memory, not from a source. Do not re-search it.
- `heimdall/source/Main.cpp` → 404 (no such file); the option table lives
  elsewhere in `heimdall/source/`. Not needed for the CLI shape above, which
  came from the README and the user guide instead.

## Next read (when the phone is back and idle)

`heimdall detect` then `heimdall print-pit` — read-only, and the PIT output is
what closes the "which slot, how big" question for a `--RECOVERY` flash.

## Sources

- https://github.com/Benjamin-Dobell/Heimdall
- https://raw.githubusercontent.com/Benjamin-Dobell/Heimdall/master/README.md
- https://raw.githubusercontent.com/Benjamin-Dobell/Heimdall/master/Win32/README.txt
- https://github.com/Benjamin-Dobell/Heimdall/issues/547 (v2.1.0, Grimler fork, A52Q)
- https://github.com/Benjamin-Dobell/Heimdall/issues/548 (A14 5G upload failure)
- https://github.com/Benjamin-Dobell/Heimdall/pull/555 (no partition-size check)
- https://github.com/jeffjose/heimdall-userguide
- https://github.com/amo13/Heimdall (fork with crucial PRs merged)
- https://git.sr.ht/~grimler/Heimdall (named in the v2.1.0 banner and PR #555)

## H20 - building heimdall on THIS Windows box (2026-10-10, "build from the tarball")

Input: `C:\Users\ilya\code\platform-tools\Heimdall-v2.2.2.tar.gz` (source, extracted to
`%TEMP%\opencode\heimdall-src\Heimdall-v2.2.2`). It is v2.2.2 (newer than the v2.1.0 the
notes analysed; same lineage).

FACT the MSVC route fails at configure: `heimdall/CMakeLists.txt:5-6` does
`find_package(PkgConfig REQUIRED)` + `pkg_check_modules(libusb REQUIRED libusb-1.0)`.
This box has no `pkg-config`, no `libusb` headers, no MinGW. cmake (Kitware 4.4.4, winget)
configures with MSVC 19.51 then dies `Could NOT find PkgConfig`.

FACT the documented Windows route (Win32/README.txt Appendix B,
https://raw.githubusercontent.com/Benjamin-Dobell/Heimdall/master/Win32/README.txt) is
**MSYS2 + MinGW-W64**: `pacman -S mingw-w64-x86_64-{cmake,libusb,clang,qt5-static,make}`
then `cmake -G "MSYS Makefiles"` + `make`. That is the reliable path, not MSVC.

OBSERVATION a 64-bit `libusb-1.0.dll` exists at `C:\Program Files (x86)\Steam\libusb-1.0.dll`
(coff-x86-64, exports the full libusb-1.0 API incl. every symbol heimdall's BridgeManager.cpp
calls: init/open/get_device_list/get_device_descriptor/get_config_descriptor/bulk_transfer/
claim_interface/release_interface/set_option/free_device_list/exit/close/ref/unref). It ships
NO headers, so it only helps at link/runtime, not compile. Not used by the MSYS2 route.

DECISION: build via MSYS2/MinGW (the upstream route). heimdall-frontend (Qt) disabled
(`-DDISABLE_FRONTEND=ON`); only the CLI `heimdall` is needed (detect/print-pit/flash).

## H21 - prebuilt Windows heimdall (abandoned the MSYS2 build; user wanted a binary)

FACT the MSYS2/pacman route stalled (mirror SSL errors + core-upgrade restart). Abandoned.
Fetched instead from the `amo13/Heimdall` GitHub release **v1.4.2** (the fork with crucial
PRs merged; assets confirmed via api.github.com):
- `heimdall-windows.exe` 856 483 B -> `%TEMP%\opencode\heimdall-win\heimdall.exe`
- `dlls.zip` 978 082 B -> libgcc_s_seh-1, libstdc++-6, libwinpthread-1, libzstd, zlib1
  (no libusb dll in the zip; the exe's imports are KERNEL32/msvcrt/libstdc++-6 only, so
  libusb is statically linked in this v1.4.2 build).
VERIFIED running: `heimdall.exe version` -> `v1.4.2` exit 0. `heimdall.exe detect` ->
`ERROR: Failed to detect compatible download-mode device` exit 1 - correct, because the
S10e is in **normal Android**, not download/loke mode. Tool works; device mode is the gate.

CAUTION (version): this is **v1.4.2**, older than the v2.1.0 the notes credit for modern
Samsungs (#547). v1.4.2's Odin-protocol handling on a 2019 Exynos 9820 is UNTESTED here.
`detect`/`print-pit` are read-only and safe to try once the phone is in download mode; the
`flash` write is the destructive step and still needs explicit consent + the AVB vbmeta
ordering (T07). If v1.4.2 cannot talk to this loke stub, fall back to the v2.1.0/v2.2.2
source build (MSYS2) or a newer prebuilt.

## H22 - v2.2.2 built via MSYS2/MinGW; the real blocker is the Windows USB driver

FACT the MSYS2 pacman transaction had actually finished (mingw-w64-x86_64 cmake/gcc/clang/make/
libusb/pkgconf all installed). Built the v2.2.2 tarball: `cmake -G "MSYS Makefiles"
-DCMAKE_BUILD_TYPE=Release -DDISABLE_FRONTEND=ON` + `make` -> **`hbuild/bin/heimdall.exe`**
(GNU 16.2.0, libusb-1.0 v1.0.30, exit 0).
FACT `heimdall.exe version` -> v2.2.2; `detect` -> `Device detected`. But `print-pit` fails:
`libusb_open open 2.10 returns -5` (LIBUSB_ERROR_NOT_FOUND). Debug shows
`[get_guid] no DeviceInterfaceGUID registered for ...` on the Samsung device
(bus 2 addr 10 = `USB\VID_04E8&PID_685D`, 04e8 = Samsung). v1.4.2 failed the same device with
-12 (`winusb_open ... unrecognized device driver`, driver `rtucx22x64`).
ROOT CAUSE (fact): Windows is driving the phone's download-mode interface with **Samsung's
proprietary driver**, which registers no `DeviceInterfaceGUID`; libusb's WinUSB backend needs
that GUID to open the endpoint. Neither heimdall version can open it until the driver is
replaced.
FIX (documented, Win32/README.txt "Driver Installation Instructions"): run **zadig** with the
phone in download mode, choose the Samsung USB Composite Device, install the **WinUSB** driver
(Option -> List All Devices -> Install this driver anyway). PC-side driver change, reversible,
NOT a write to the phone. After that, `print-pit`/`flash` should open the endpoint.
STATUS: heimdall tooling is ready; the phone's Windows driver is the remaining gate before any
flash. zadig not yet installed.

## H24 - the zadig detour was the WRONG path; the repo uses the Samsung driver natively

CORRECTION (from code, not memory): the installer's own TWRP flasher is `OdinTwrpFlasher`
(`AppServices.cs:94-106`, preferred) over the **official Samsung USB Driver for Mobile Phones**
(`HostPreflight.cs:103`). zadig/WinUSB was only needed because I ran *standalone heimdall*
(the fallback `HeimdallTwrpFlasher`). The native route needs no zadig.
FACT the user reverted the phone to **stock** (TWRP no longer installed). Reference artefacts
on the machine: `C:\Users\ilya\code\twrp-3.7.0_9-2-beyond0lte.img.tar` (56 514 560 B, matches
`TwrpProfiles.cs:37`) and `C:\Users\ilya\code\vbmeta_disabled.tar` (2048 B).
AVB order (Team Win, `StageCatalog.cs:138-139`): verity-disabled **vbmeta first**, then TWRP to
RECOVERY (sda15). This disk's vbmeta is LBA 83 584..83 599.
NEW PARKING-LOT TASK **P04** (`tasks/P04-vbmeta-twrp-flash.md`) added to guide users through
vbmeta→TWRP on a stock S10e (native Odin flasher, no zadig); progress.md updated.
PLAN: user flashes vbmeta+TWRP manually, reboots to TWRP; then I write the UEFI candidate to
the recovery slot and boot it (the first-boot observation).

## H23 - zadig 2.9 (Akeo / pbatard-libwdi) is a GUI app; user drives it

FACT downloaded `zadig-2.9.exe` (5 334 088 B) from pbatard/libwdi release v1.5.1
(https://github.com/pbatard/libwdi/releases/download/v1.5.1/zadig-2.9.exe). Running it opened
its **GUI** (no usable CLI from the packed binary; `--help` produced nothing). So the WinUSB
patch is a manual click-through by the user, with the phone already in download mode.
Steps given to the user: Options -> List All Devices -> select the Samsung composite device ->
install the **WinUSB** driver (accept the "unable to verify publisher" prompt) -> it re-launches
elevated. After that the device exposes a DeviceInterfaceGUID and heimdall's libusb can open
2.10. Then retry print-pit.
