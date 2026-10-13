# Adversarial review — `~/.config/opencode/skills/research/`

Reviewer: subagent, 2026-10-12. Stance: the skill is wrong until proven otherwise.
I do **not** edit the skill; findings only.

## 0. What is under review (inventory, measured)

| File | Lines |
|---|---|
| `SKILL.md` | 217 |
| `references/evidence-sources.md` | 144 |
| `references/windows-driver-shapes.md` | 130 |
| `references/docs-and-commits.md` | 123 |
| `references/review-checklist.md` | 89 |
| `assets/{design,driver-readme,notes,task}-template.md` | 57 / 49 / 54 / 35 |
| `scripts/scaffold.py` | 94 |
| `commands/research.md` | 14 |

Total ≈ 1006 lines.

Repo state at review time: branch `drivers`, clean tree, HEAD `6cdf8de`
("HANDOFF 25: D4c Bluetooth session restart point"). Recent commit subjects use
`D4c …` / `M12 …` / `D8 …` prefixes, **not** `research:` — checked with
`git log --oneline -25`.

> **Hash note (2026-10-13):** the D4c and D9 arcs were later collapsed to one
> commit each, so every hash in this file is a **pre-collapse** hash and no longer
> reachable from `drivers`. The old history is kept under the tags `d4c-pre-squash`
> and `d9-pre-squash`; the findings they record are unchanged.

---

## 1. BLOCKER — the skill's own ASL example contains an invented constant

`references/windows-driver-shapes.md:28-38` is the one code block an agent will
copy. It is wrong against the shipped DSDT:

```asl
Memory32Fixed (ReadWrite, 0x13EC0000, 0x1000)    // CFG window   <- skill
Memory32Fixed (ReadWrite, 0x14000000, 0x0100000) // MEM window   <- skill
```

Real `firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl:348-349`:

```asl
Memory32Fixed (ReadWrite, 0x14FFE000, 0x00002000)  // CFG0 window
Memory32Fixed (ReadWrite, 0x14000000, 0x00FF0000)  // MEM window (EP BAR0)
```

Checked mechanically:

- `grep -rn 13EC0000` → the only hits are `EXYNOS_PCIE0_IA_BASE`
  (`Exynos9820.h:452`, "I/A microprogram sequencer"), the `Ia` pointer in
  `Beyond0LtePcieDxe/PcieInternal.h:19`, and `notes/m12-wifi-chip.md:75`. It is
  **not** a config window.
- `grep -rn 14FFE000` → `Exynos9820.h:450 EXYNOS_PCIE0_CFG_BASE … VERIFIED (reg
  "config": CFG0 iATU window)`, `Exynos9820Wlan.h:31 WLAN_RES_CFG_WINDOW /*
  0x14FFE000 +0x2000 */`, Dsdt.asl:348.
- MEM size: skill `0x0100000` (1 MiB) vs real `0x00FF0000` (16 MiB − 4 KiB).

Why it is a BLOCKER and not a nitpick: `0x13EC0000` is the **IA block**, which the
same file says two bullets later must stay *out* of `_CRS` — "the same role-latch
logic hides ELBI/DBI/PHY/IA from WLAN" (`windows-driver-shapes.md:48`), and
Dsdt.asl:336-337 says "The ELBI/DBI/PHY/IA windows are deliberately NOT here".
So the reference file contradicts itself, and the example hands the driver the
one window the design forbids. This is precisely the failure the skill's own
first axiom (SKILL.md:29 "A constant you did not read is a constant you
invented") says is the most expensive class. The sample also omits `_UID` and
`_STA`, which the real device has.

Fix: replace the block with the verbatim DSDT excerpt (0x14FFE000/0x2000,
0x14000000/0xFF0000, `_UID`, `_STA`, and the `// _CRS ORDER IS A BINDING CONTRACT`
comment), and cite `Dsdt.asl:343-357`.

---

## 2. Facts asserted by the skill — what I checked, what held, what did not

### Verified correct (mechanically, against the repo)

| Claim (file:line) | Check | Result |
|---|---|---|
| `tools/dump-tools/` has `fdt.py sbootimg.py sparse_img.py dtsift.py xref.py kallsyms.py test_fdt.py` (evidence-sources.md:24-25) | `ls tools/dump-tools/` | **all 7 present**, no extras beyond `__pycache__`/`.venv` |
| `fdt.py` "handles Samsung's v40 DTB layout" (:24) | `grep -n version tools/dump-tools/fdt.py` | **holds** — `fdt.py:105` "Samsung downstream dtc variant (seen in the 9820 dt.img, version 40)" |
| `dumps/hhxj6/` = "Android 12, G970FXXUHHXJ6" (:12) | `notes/cpu-driver-9820.md:38` tar name `AP_G970FXXUHHXJ6_…_OS12_…` | **holds** |
| `base.dtb`, `base.dts`, `ov*.dts`, `kernel`, `cm.bin`, `src/`, PIT | `ls -la dumps/hhxj6/` | **present** (`ov00.dts`, `BEYOND0LTE_EUR_OPEN.pit`) |
| header path `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` (SKILL.md:100, docs-and-commits.md:16, notes-template.md:40) | `ls` | **exists**, 50 385 B |
| `_HID` values `SAMS9825` WLAN / `SAMS9826` BT / `SAMS9821` GPIO (windows-driver-shapes.md:30, docs-and-commits.md:39) | `grep -rn SAMS98` | **all three** in Dsdt.asl:343/369/407 and in the drivers' `.h`/`.inf` |
| GSIV 285 WLAN, GSIV 408 BT | Dsdt.asl:353, :385; `Exynos9820.h` | **both correct** |
| DWC3 globals `+0xC100` deliberately absent from `XHC0._CRS` (windows-driver-shapes.md:47) | `Dsdt.asl:303` | **correct** |
| `usbxhci.inf` binds `ACPI\PNP0D10`/`PNP0D15` (:14) | `Dsdt.asl:299-313` | **correct** |
| 9810 ALIVE GPIO base `0x14050000` (review-checklist.md:16) | `Exynos9810Pkg/…/Exynos9810.h:193`, `Star2LtePkg/AcpiTables/Dsdt.asl:247` | **correct**; 9820 is `0x15850000` (Dsdt.asl:411) |
| `0xCC000000` = 9810 fb base, unusable on 9820; `0xFED1xxxx` inside `debug_snapshot` (evidence-sources.md:132-133) | `notes/s10e-live-reads.md:156,161,262`, `notes/m8-libs.md:11,26` | **correct** |
| 9810 RKP range `0x80094000-0x828DBFFF` has no 9820 equivalent (:134) | `notes/m8-dscfdf.md:117` | **correct, verbatim** |
| QCH ids in `cmucal-{vclk,node,qch}.h` are enum positions, not addresses (:79) | `notes/s10e-live-reads.md:167` | **correct** |
| mirrors `LineageOS/android_kernel_samsung_exynos9820`, `CruelKernel/samsung-exynos9820` @ `cruel-HVJ5-v5.3`, `GoRhanHee/…` @ `gorhanhee`, `rmem_s5100_dual.dtsi` (:76-78) | `notes/s10e-live-reads.md:134,213-220`; `notes/bt-driver.md:210` | **all four used in the repo** |
| `serialhcibus` + `cywbtserialbus` are MS-PL (windows-driver-shapes.md:116-117, 125) | GitHub license API for `microsoft/Windows-driver-samples` and `worproject/cywbtserialbus` | **both `spdx_id: MS-PL`** — claim holds |
| Samsung UART is not 16550 → no in-box serial stack (windows-driver-shapes.md:86) | `notes/bt-driver.md:174`, `D4c-BT-DESIGN.md:53` | **correct** |
| "the touch package's gesture tests" host mock exists (SKILL.md:144) | `drivers/S6SY761Touch/tests/TouchGestureTests.c`, `drivers/S6SY761TouchBeyond0/tests/` | **exists** (but see §14 — it is a `.vcxproj`, Windows-only) |
| 260 commits mis-attributed to ntdevlabs (docs-and-commits.md:53-54) | `notes/git-identity.md:5-7` | **correct**; `git config user.name/email` is now `ilyatbn` / `ilyatbn@users.noreply.github.com` ✓ |
| D7 exists (evidence-sources.md:140 lists D1/D3/D4b/D4c/D5/D6/D7/D8) | `DRIVERS-MILESTONES.md:141` | **holds** — D7 is a task section, not a milestone-table row |

### Wrong, stale, or unverifiable facts

**(a) `dumps/hhxj6/vendor.raw` is not where the skill says it is.**
evidence-sources.md:19 lists `vendor.raw (ext4)` as a member of the dump dir.
Actual: `dumps/hhxj6/vendor/vendor.raw` (one level deeper, inside `vendor/`).
`notes/bt-driver.md:209` cites the correct path: `dumps/hhxj6/vendor/vendor.raw
(debugfs)`. An agent that runs `debugfs dumps/hhxj6/vendor.raw` fails.

**(b) "the defconfig" is listed as a local dump artefact — it is not one.**
evidence-sources.md:20 puts `the defconfig` in the table of "Typical contents of
a dump dir (`dumps/hhxj6/`)" and credits it with the finding "stock Android never
speaks HCI in-kernel". Checked:
- `find dumps -iname '*defconfig*'` → **nothing**.
- `python3` scan of `dumps/hhxj6/kernel` for `IKCONFIG_STRICT` /
  `CONFIG_BT_HCIUART` → **not found** (no embedded IKCONFIG to extract).
- The finding actually came from rung 3: `notes/bt-driver.md:68` cites
  `arch/arm64/configs/exynos9820-beyond0lte_defconfig` from the LineageOS mirror
  (`notes/bt-driver.md:211`, `notes/m12-wifi-chip.md:41-42`).
So the row is in the wrong ladder rung and the credit is misattributed. Same
error in `HANDOFF §24` ("`CONFIG_BCMDHD_PCIE=y` in the shipping defconfig") — the
defconfig is a mirror read, not a dump read. Fix: move the row to §3 (mirrors) or
state "from the mirror's `arch/arm64/configs/…_defconfig`; the shipped `kernel`
Image has no IKCONFIG, so the defconfig is **not** available locally".

**(c) "PIT / GPT" (:21).** `find dumps -iname '*gpt*'` → nothing. Only
`BEYOND0LTE_EUR_OPEN.pit`. The GPT reads in this repo came from the device
(`notes/s10e-partitions.md`, `notes/pit-geometry.md`), not from `dumps/`.

**(d) `adb` path is Windows-only and stale for the box the agent runs on.**
evidence-sources.md:37 repeats AGENTS.md §8's `C:\Users\ilya\code\platform-tools\adb.exe`
"invoked with `&`". Sessions run on **Linux** (`uname` = linux, cwd
`/home/ilya/exynos-woa`), where `which adb` → **`/usr/bin/adb`, on PATH**. The
skill gives the agent a PowerShell incantation that cannot run here and no
Linux path at all. It also never says "check the host you are on".

**(e) "there is exactly one S10e here" (:38) is currently false in practice.**
`HANDOFF §24` and `§25` both record "the phone was NOT attached this session
(`adb devices` empty)". Repo AGENTS.md §8 asserts it *is* connected
(`RF8M21HYDSZ`). The skill repeats the assertion without the escape hatch the
repo actually uses. See §11 (gap: no device-unavailable branch in the loop).

**(f) `SimpleAudioSample` — not present.** (Named in my brief; `grep -rn
SimpleAudioSample` over the skill → 0 hits.) Nothing to verify; noting it so the
next reader does not go looking.

**(g) CORRECTION — I wrote three "no hits" claims without running the greps.**
The first draft of this section asserted `grep -rn Armada` and `grep -rn
ExynosAuto` returned nothing. They do not. Re-run properly:

- **Marvell Armada70x0** (evidence-sources.md:104) — **verified in repo**:
  `notes/d1b-usb-vbus-hcd.md:205,209,214,272,276`, with the exact upstream URL
  `MarvellEmbeddedProcessors/edk2-open-platform/…/Armada70x0/Dsdt.asl`.
- **ExynosAuto 2025 upstream attempt** (:130-131) — **verified in repo**:
  `notes/d8-audio-linux.md:386-387` (LKML "[PATCH 0/9] Add ExynosAuto ABOX …",
  Eunwoo Kim, ExynosAutoV920) and `DRIVERS-MILESTONES.md:156` states the same
  verdict verbatim ("ABOX **not mainlined** … 2025 upstream attempt = ExynosAuto
  only").
- **NXP Layerscape `RegisterNonDiscoverableMmioDevice(Xhci,…)`** (:105) —
  verified: `notes/d1b-usb-vbus-hcd.md`, `DRIVERS-MILESTONES.md:66`.
- **Mu `Status.md` ❌ list** (:101) — USB-host ❌ verified
  (`DRIVERS-MILESTONES.md:21`); `S5E9820Pkg`/`UsbfnDwc3Dxe`/`SimpleFbDxe` all
  appear in the notes (5/3/3 files).
- **Mali WDDM dead end** (:126-128) — verified: `notes/d5-gpu-display.md:59-60`
  records the Update Catalog query and its zero-result output, :247 states the
  verdict.
- **No USB WiFi for ARM64** (:129) — verified: `notes/d4-usb-wifi.md:19-31,78`.
- **`sonic011gamer/edk2-exynos`** (:102) — appears in 10 notes/docs files ✓.

So the "known dead ends" section of the skill is **sound** — every one I checked
mechanically traces to a repo note that records the query that established it.
That is the strongest part of the skill.

Still open (I did not re-run these, they are inherited from the repo):
- windows-driver-shapes.md:18 "Realtek/ASIX confirmed ARM64" — the supporting
  chip-by-chip scan is in `notes/d4-usb-wifi.md`; I did not repeat the scan.
- evidence-sources.md:126 "Arm's downloads are Linux/Android only" — not checked.

The lesson applies to the skill itself: this is the exact defect
`review-checklist.md:21-23` warns about, and it is why the checklist's "spot-check
every `path:line` by opening it" rule is right.

---

## 3. Contradictions and drift against the repo's own rules

The skill says "This skill is the default, not the authority" (SKILL.md:37) and
lists the repo files that win. Where they actually disagree:

**(1) BLOCKER-adjacent: the `research:` commit prefix does not exist in this repo.**
`git log --grep='^research:' --oneline | wc -l` → **0** out of **298** commits.
The real convention, from `git log --format='%s' | awk '{print $1}' | sort | uniq -c`:
`docs:` 32, `M8:` 28, `notes:` 25+15, `M9:` 11, `D8` 9, `M12` 8, `K10` 7, `D4c` 6,
`D1` 6, `HANDOFF` 6, `review` 5, `M6:` 4 … i.e. **`<ID> <area>: …`**.
The D4c arc — the skill's own model — landed as:
`D4c/BT research: …`, `D4c research review: …`, `D4c constants: …`,
`D4c design: …`, `D4c DSDT: …`, `D4c DSDT review: …`, `D4c Windows half: …`.
Review fixes are `D4c research review:` / `M12 constants review:` /
`M12 driver review:` / `M12 review follow-ups:` — **not** the skill's
`research: review fix:` (SKILL.md:160, review-checklist.md:6, docs-and-commits.md:41).
Consequence: the skill's grep `git log --grep='^research:'` (docs-and-commits.md:28)
returns nothing for every arc already in the tree, so "the arc is greppable" is
false, and the next session searching `^D4c` misses everything this skill writes.
The skill also invents a third style with an em-dash (`research: D9 camera — …`)
that appears nowhere in the history.
Fix: make the prefix `<ID> <area>:` and say the ID is the milestone/task ID, with
the real D4c ladder as the example.

**(2) MAJOR: the repo mandates a subagent for research; the skill's loop does not.**
`DRIVERS-MILESTONES.md:172-173` ("Working rules for this file"): "Research tasks go
to a **subagent** that appends findings (with source URLs) to the named notes file
**as it works**; the implementation pass reads only that file." Global
`~/.config/opencode/AGENTS.md` has a "Subagents" section with the same shape.
SKILL.md's loop (§0-§6) has one agent do frame→evidence→constants→design→implement
and never mentions delegation or the "implementation reads only the notes file"
rule. That is a direct omission of a repo rule the skill claims to defer to.

**(3) MAJOR: no branch policy, and "never push" contradicts how this tree is
actually published.** SKILL.md:199 "Never push, never force-push, never rewrite
history unless asked." Measured: `git rev-parse HEAD origin/drivers` → both
`6cdf8de`, and `git rev-list --count origin/drivers..HEAD` → **0**. The drivers
branch is pushed every session. Meanwhile repo `AGENTS.md` §10 says `main`'s
ntdevlabs commits "**get rewritten** after the drivers→main merge. Do not 'fix'
them early" — a rewrite is *planned*, and the skill's blanket ban plus silence on
branches leaves an agent unable to decide (a) which branch to start on — the tree
is on `drivers`, `main` is 86d37da behind — or (b) whether publishing is allowed.
Fix: one line — "work on the branch the plan of record is on (`git status -sb`);
do not switch to `main`; do not push unless the repo rules or the user say so."

**(4) MAJOR: task-file IDs collide with the repo's ID namespaces.**
`scaffold.py --id D9` writes `tasks/D9-r1-sensor-id.md` and prints
`- [ ] **D9** — … → tasks/D9-r1-sensor-id.md`. Existing task files are
`P01..P04`, `T01..T09`, `W01..W02` — **no task file uses a D or M ID**; D/M IDs
live only in the milestone tables. `progress.md:3-4` says "One line per task; the
detail lives in `tasks/<ID>-*.md`", which reads as if the milestone ID is the task
ID. The skill never explains the three namespaces (M1..M15 boot milestones,
D1..D8 driver milestones, T/W/P task files) or how to pick a new one — see §11.

**(5) MINOR: "one task file per rung" is heavier than the repo's practice.**
SKILL.md:170 "`tasks/<ID>-<slug>.md` per rung". The M12/D4b arc has a 5-rung
ladder and produced **two** task files (W01 = R1, W02 = the live reads); the D4c
arc produced **zero** task files and put the rungs in `D4c-BT-DESIGN.md` +
`DRIVERS-MILESTONES.md` rows. Following the skill literally multiplies files the
plan of record does not use.

**(6) MINOR: the scaffold's milestone row does not fit either table.**
It prints `| Camera sensors on the S10e | todo | research `notes/…` |`. The
Milestones table is 4 columns `| # | Milestone | State | Blocked on |`
(`DRIVERS-MILESTONES.md:46`); task tables are 3 columns `| Task | State | Notes |`
(:62). The row matches the task-table width but not its semantics (first cell must
be the task text, not the subsystem title), and it prints bare `todo` where the
repo writes `**todo**`/`**doing**`/`**done**` (87 `**done**` occurrences).

### Where the skill and repo genuinely agree (checked, so nobody re-litigates)

- `todo → doing → done` in the same commit as the work — HANDOFF §2 rule 1,
  `DRIVERS-MILESTONES.md:170-171`, SKILL.md:44/190 ✓.
- `UNVERIFIED` naming — HANDOFF §2 rule 4, `DRIVERS-MILESTONES.md:174-175`,
  SKILL.md:104-106 ✓.
- `Guided` / `Experimental` / `NotImplemented` — all three exist in
  `installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs:22,28,31` ✓
  (docs-and-commits.md:69 is accurate).
- `needs-phone` / `needs-windows` / `needs-boot` — 16/12/12 occurrences across
  docs/notes/tasks ✓, and HANDOFF §2 rule 7 is the same idea.
- 9810 packages byte-unchanged — AGENTS.md §6 vs SKILL.md:202-203 ✓.
- No powered hub / no media writes until Windows boots — HANDOFF §2 rules 5-6 vs
  SKILL.md:197-198, evidence-sources.md:66-67 ✓.
- Harsh per-commit review with follow-up commits — AGENTS.md §4 vs SKILL.md §5 ✓.
- Restart point before the last commit — AGENTS.md §5 vs SKILL.md:175-177 ✓.

---

## 4. `scripts/scaffold.py` vs the templates it renders

Tested in `/tmp/opencode/scaffold-test2` against a fake tree
(`AGENTS.md` + `docs/exynos9820-port/`); the real repo was not touched.

**Works:** creates `notes/<topic>.md`, `tasks/<ID>-<slug>.md`,
`docs/exynos9820-port/<design>.md`; refuses to overwrite on re-run ("exists,
untouched", exit 0); auto-detection correctly refuses a non-port tree
("no plan-of-record tree found …; pass --repo"); slug is sanitised
(`re.sub(r"[^a-z0-9]+","-")`); prints a `progress.md` line in the repo's real
format (`- [ ] **D9** — Title → `tasks/D9-r1-sensor-id.md``).

**Bug 1 — the design template's placeholders are not the ones the script fills.**
`subs` = `{Subsystem, ID, date, topic}` (scaffold.py:61). `design-template.md:1`
is `# <ID> — <subsystem> on the <device>: design` — lowercase `<subsystem>` and
`<device>` are **never substituted**, so after passing `--title "Camera sensors
on the S10e"` the rendered H1 is literally
`# D9 — <subsystem> on the <device>: design`. Same mismatch for `<SoC>`/`<block>`
(:4), `<Pkg>`/`<Name>` (:27), and — the avoidable one — `<slug>` (:57): the script
knows the slug and still renders `tasks/D9-<slug>.md` while the file it created is
`tasks/D9-r1-sensor-id.md`. Fix: add `subsystem`/`device`/`slug` to `subs`, or
rename the template's `<subsystem>`→`<Subsystem>` and fill `<slug>`.

**Bug 2 — `--repo` bypasses the plan-of-record guard.** `find_repo()` checks for
`AGENTS.md` + `docs/*-port/` (scaffold.py:22-28), but `--repo` skips it entirely
(:58). Pointed at a directory with only an `AGENTS.md`, it happily wrote
`notes/` and `tasks/` into a non-port tree.

**Bug 3 — `--design` crashes with a traceback on such a tree.**
`next(p for p in (repo/"docs").iterdir() …)` (:73-74) raises
`FileNotFoundError: … '/tmp/opencode/scaffold-test2/bad/docs'`, exit 1, *after*
notes and tasks were already written — a half-scaffolded topic.

**Bug 4 — `--id` is interpolated into a path unsanitised.**
`--id ../../ESCAPE` wrote `/tmp/opencode/scaffold-test2/ESCAPE-trav.md`, i.e.
**outside the repo**. `--topic a/b` created `notes/a/b.md`. Low severity (the
caller is the agent itself), but the script's docstring promise "Scaffold a
research topic in an Exynos/WoA port tree" is not enforced.

**Gap — `assets/driver-readme-template.md` is unreachable from the script.**
SKILL.md:217 says scaffold "creates `notes/<topic>.md` + `tasks/<ID>-<slug>.md`
and prints the `progress.md` / milestone lines", but SKILL.md:216 lists the
driver-README among the templates; there is no `--driver` flag, so the one
template that encodes the bind contract and license posture is hand-copied with
no tooling. Either add `--driver <name>` or drop the implication.

**Cosmetic:** the printed milestone row is unbolded `todo` and 3 cells wide
against a 4-column Milestones table (see §3.6); the "Commit the scaffold on its
own" message hard-codes the `research:` prefix that §3.1 shows the repo does not
use.

---

## 5. Gaps — what an agent following this skill gets wrong or skips

**(G1) MAJOR — the "Known dead ends" list is a stale snapshot with no reopening
rule, and this repo has already been burned by exactly that.**
`evidence-sources.md:124` heads the section "do not re-walk without new
evidence". But the repo's most expensive recent correction *was* re-walking a
dead end: commit `aaff4b6` "M12/D4b research: the S10e WLAN is a BCM4375 on
PCIe0@13ED0000 — **the dead-end verdict was wrong**", and
`DRIVERS-MILESTONES.md:24` "the old 'no DT device node = dead end' reading was
about the platform glue only and is **corrected 2026-10-11**". The skill's §7
bullets carry **no date, no establishing note, and no "what would reopen this"**
— the opposite of the skill's own rule at :107-108 ("write it down with the
searches that established it"). Concretely, §7's GPU bullet (:126-128) is already
behind the repo: `DRIVERS-MILESTONES.md:54` shows acceleration **reopened as D5b**
because "kbase r10p0 source is already in the 9820 kernel"
(`notes/d5-gpu-display.md:119-121,321`), which the skill's "closed DDK = dead end
under Arm's EULA" line does not mention. An agent that trusts §7 can decline to
research WLAN or GPU at all — a false negative that costs a whole feature.
Fix: give every §7 bullet `(<established by notes/x.md §N, <date>>)` plus a
"reopens if <specific read>" clause, and add one line: "a dead end is a claim
about a *question*, not about a subsystem — restate the exact claim it negates
before refusing to look."

**(G2) MAJOR — no branch for "the phone is not attached".**
The loop (§0-§6) assumes device reads are available; §1 even says "Device reads
are cheap and safe; use them before guessing". Reality per
`HANDOFF §24`/`§25`: both sessions recorded "`adb devices` empty". The only
place the skill handles it is `task-template.md:4-6` ("if you checked and it was
not attached, say so"). SKILL.md never says: run `adb devices` first, and if
empty, mark the open items `needs-phone`, queue the exact command, and continue
with the dump/mirror rungs — which is what the repo actually did. Add it to §1.

**(G3) MAJOR — no branch for "no prior art exists".**
The ladder's rung 5 is prior-art ports. For several subsystems the repo's answer
is "there is none": `DRIVERS-MILESTONES.md:157` "Mu-Silicium audio ❌ on every
Exynos target — **no prior art to copy**"; `:120` WWAN "no inbox Exynos path".
The skill never says what to do then (fall back to the class-extension contract
+ the vendor sequence, and say so in the design's license/prior-art section).
An agent hitting an empty rung 5 has no instruction and tends to invent a shape.

**(G4) MAJOR — how to pick an ID is never explained, and there are three
namespaces.** `MILESTONES.md` has M1..M15 (boot stream);
`DRIVERS-MILESTONES.md` has D1,D2,D3,D4,D4b,D4c,D5,D6,D7,D8 (driver stream, and
it says at :4-5 it "expands the M10–M15 rows"); `tasks/` uses T01..T09,
W01..W02, P01..P04. SKILL.md:44 and docs-and-commits.md:14 name the two milestone
files but never say which one a task belongs to, that D-rows expand M-rows, or
how a new subsystem gets numbered (is the next one D9? M16? T10?). `scaffold.py`
demands `--id` with no guidance, and its example (`D9`) invents a number that does
not exist yet. This is the single most likely place for an agent to make a
plan-of-record mess.

**(G5) MINOR-MAJOR — the build recipe is asserted but not pointed at.**
SKILL.md:140-141 says a Linux verification build "is allowed and expected here",
and §4 tells the agent to get compile evidence — but nothing cites
`notes/d1-build-env.md`, which is where the non-obvious recipe lives (edk2
`--depth=1` clone into the gitignored `firmware/edk2`, the specific submodules
that must be initialised, the no-sudo constraints, and the fact that the helper
script `/tmp/opencode/build-uefi-linux.sh` is **ephemeral** and must be
regenerated from the notes). Without that pointer, every session re-derives the
toolchain.

**(G6) MINOR — the skill's ACPI-only framing hides the third Windows shape the
repo actually uses for audio.** `windows-driver-shapes.md:25` "Devices on this
SoC are **ACPI-described, not PCI-enumerated**", and
`driver-readme-template.md:9` hard-codes "binding `ACPI\<HID>`". But the repo's
chosen audio skeleton is **root-enumerated**: `notes/d8-audio-windows.md:32`
"Hardware ID = `ROOT\SimpleAudioSample` — a *root-enumerated* device; no PCI, no
ACPI", and `DRIVERS-MILESTONES.md:164` names SimpleAudioSample as the portcls
basis. The skill never mentions `SimpleAudioSample` (grep over the skill: 0 hits)
nor ROOT enumeration, so an agent scopes audio as an ACPI-bound miniport and
fights the class extension.

**(G7) MINOR — evidence that lives in gitignored build output is treated as
grep-able.** docs-and-commits.md:81 asks for "the grep of the built
`Dsdt.aml`/`FVMAIN.Fv`". `.gitignore` has `Build/`, `edk2/`, `*.fd` and
`git ls-files firmware/edk2` → **0 tracked files**, so that grep is session-local
and cannot be re-run by the next session. The skill should say: record the command
*and its output line* in the notes (as `notes/bt-driver.md:200` does: "`Dsdt.aml`
contains `BTH0`+`SAMS9826` (byte grep)"), not just the command.

**(G8) MINOR — no mechanical check for the leakage hard stop.** SKILL.md:204-206
forbids blobs/IMEIs/hashes, but never points at `tools/release/scan.py`, which
`CONTRIBUTING.md` says CI runs and "will reject the obvious cases". One line
("run `python tools/release/scan.py` before the arc's last commit") turns a
hard stop into a gate — exactly the skill's own "prove the gate bites" philosophy
(SKILL.md:111-113).

**(G9) MINOR — `MAP.md` is cited but is stale and 9810-centric.**
evidence-sources.md:144 lists `MAP.md` as a consulted source. `MAP.md:1-8` says
"Generated 2026-10-08" and "**Project:** run Windows 11 ARM64 on the Samsung
Galaxy S9+ **SM-G965F** (Exynos 9810, `star2lte`)". Read naively it contradicts
SKILL.md:14 ("Current project: Exynos 9820, Galaxy S10e"). Same for
`CONTRIBUTING.md`'s golden-rule device. The skill should mark MAP.md as
9810-era context, not current scope.

**(G10) MINOR — chunking is one example, not a rule.** SKILL.md:70-73 ("A camera
'driver' is usually five tasks") gives an example but no test for when to split,
and never mentions the session todo list that repo `AGENTS.md` §3 points at
("add them to the todo list"). Nor does it say what "smaller than the work"
means in output terms, which is the actual reason for the rule in this repo
(`AGENTS.md` §1: a long single response is unrecoverable).

**(G11) MINOR — research-only vs build is decided from wording only.**
SKILL.md:63-66 decides the mode from the user's phrasing. It never says to check
the milestone row first: if the row is already `done (code; not built —
needs-windows)`, "build the driver" means *compile it*, not write it again. The
repo's next-order-of-work for both D4b and D4c is literally "needs-windows:
msbuild …" (`HANDOFF §24`, `§25`). Mode selection should read the plan of record,
not just the sentence.

---

## 6. Bloat, duplication, and usability

**6.1 Size.** SKILL.md is 217 lines / ~11 kB; the four references add 486 lines.
SKILL.md's length is about right for a workflow this involved — the problem is not
length, it is that **SKILL.md re-states the references**, so there are two places
to keep in sync and the skill's own §1 says the repo wins.

**6.2 Duplication measured** (files a phrase appears in, `grep -ril` over the
skill): `UNVERIFIED` 6/7 files, `rung` 6, `GPL` 6, `MS-PL` 5, `NOTICE` 5,
`citation` 5, `inbox` 4, `two halves` 4, `needs-windows` 4, `dead end` 4,
`milestone row` 4, `progress.md` 4, `restart point` 2, `harsh` 2.
Concrete near-verbatim pairs (drift risk, one copy will get edited):

| SKILL.md | duplicate |
|---|---|
| §4 "A skeleton is a good skeleton when…" (147-152) | `windows-driver-shapes.md` §5 (92-104) — same four bullets, different words |
| §5 review paragraph (154-164) | `review-checklist.md` 1-7 — same rule, same example count ("five wrong source-line citations" appears at SKILL.md:164, review-checklist.md:23 and :70) |
| "Commits" (179-192) | `docs-and-commits.md` 23-54 |
| "Hard stops" (194-206) | repo `AGENTS.md` §6/§7 + `HANDOFF.md` §2 — a **third** copy of rules the skill tells the agent to read first (SKILL.md:37-45). This is exactly where the drift showed up (§3.3 "never push"). |
| §2 constants labels (97-113) | `review-checklist.md` Q1/Q5, `DRIVERS-MILESTONES.md:174-175` |
| §0 mode decision (63-66) | `commands/research.md:11-14` — same two examples, same rule, in two files |

Fix: keep SKILL.md as the loop + the hard stops **by reference** ("HANDOFF §2
rules 1-7"), and let each rule live in exactly one file.

**6.3 Dead weight.**
- `evidence-sources.md:137-144` "Sources consulted while writing this skill" —
  8 lines of build metadata for the skill itself. It cannot help a research pass,
  it goes stale the moment the repo moves, and one of its entries
  (`notes/d8-audio*.md`) is a glob, not a citation. Move it to a `CHANGELOG` or
  delete it.
- `windows-driver-shapes.md` §4 class taxonomy (76-91) overlaps
  `DRIVERS-MILESTONES.md`'s own per-D rows; it is the file most likely to go
  stale, because the repo's rows change every session. Keep the "watch out for"
  column, drop the rest or point at the milestone file.
- `docs-and-commits.md:85-95` "Notes file shape" re-describes
  `assets/notes-template.md`, which the agent will open anyway.

**6.4 Reference pointers are mostly unambiguous, with two holes.**
The §References table (210-217) maps file → "read it when", which is good. But:
- `assets/driver-readme-template.md` is **never referenced from the loop** — only
  from the table. Step 4 (implement) is when it is needed and does not name it.
- `references/docs-and-commits.md` likewise appears only in the table, though
  step 0 (notes format) and step 6 (restart-point format) depend on it.
- `scripts/scaffold.py` is cited as a bare relative path (SKILL.md:68, 217). From
  the repo cwd that is wrong — `ls scripts` in `/home/ilya/exynos-woa` → **no such
  directory**; the real path is
  `~/.config/opencode/skills/research/scripts/scaffold.py`. OpenCode does print
  the skill's folder when it loads, so this is recoverable, but the skill should
  say "run it from the skill directory".

**6.5 Templates vs the repo's real files** (the "match the format" claim):
- `notes-template.md`: `## Progress log` matches 20/51 notes ✓, `## Sources`
  34/51 ✓. But `## Constants frozen`, `## Review log`, `## Inference` appear in
  **0** notes files. In the exemplar `notes/bt-driver.md` the review log is an
  h3 inside the progress log (`:178`, `:192`) and Sources is `### Sources (B3/B4)`
  at :206 — different heading level from the template's `## Sources`.
- `task-template.md`: its headings `## Do this`, `## What it proves`,
  `## Rollback`, `## Consent / safety` appear in **0 of 15** task files. The real
  convention is `## Goal` (12), `## Done when` (13), `## Next actions` (9),
  `## Known` (4), plus per-task variants (`## What it closes`, `## If it fails`,
  `## Do NOT` in `tasks/W01-wifi-r1-linkup.md`).
  So `docs-and-commits.md:4-5` ("Match these formats exactly — a novel format is
  a format the next session does not find") is violated by the skill's own
  templates: they *are* the novel format.
- `design-template.md` is close to the real designs
  (`D4c-BT-DESIGN.md`, `M12-WIFI-DESIGN.md`: rung table, two halves, license
  posture, risks with closing reads) ✓ — the best-fitting template.
- `driver-readme-template.md` matches `drivers/Exynos9820Bt/README.md` well
  (status line, bind contract, license posture, build section) ✓.

**6.6 `scripts/scaffold.py` is not referenced by the command file at all**
(`commands/research.md` never mentions it), so the `/research` path may skip the
scaffold entirely while the auto-invoked path uses it. Two entry points, two
behaviours.

---

## 7. Triggering

Measured: description is **949 characters / 137 words** (frontmatter 1050 chars).
V1's Agent-Skills schema caps description at 1024 — it fits, with 75 chars of
headroom, so any edit that lengthens it breaks portability.
`metadata.opencode/autoinvoke: true` (SKILL.md:16) is a **no-op**: OpenCode's docs
say `autoinvoke: false` removes the skill from the model-facing list; the default
is already advertised. Harmless, but it is dead config that reads like it does
something. Separately, `name: Exynos WoA research` (SKILL.md:2) violates V1's
`^[a-z0-9]+(-[a-z0-9]+)*$` and the name==folder rule (folder is `research`); V2
derives the ID from the path so it works here — the command file correctly says
"exact id: `research`" (`commands/research.md:5`) — but the skill is not
portable to V1/Claude Code as written.

**Would it fire for the four realistic prompts?**

| Prompt | Verdict | Why |
|---|---|---|
| `/research the 4g model radio` | **yes** | `/research` is named in the description (:13) and the command exists; "modem/radio" is in the class list (:9) |
| `/research and build drivers for camera sensors` | **yes** | "camera sensors" (:9) + "write/build a driver for X" (:13) |
| `what drives the touch panel on the S10e` | **yes** | "what drives X" (:12) + "touch" (:9) + "Galaxy S10e" (:14) |
| `find the CMU gates for the SPI bus` | **yes** | "find the constants for X" (:12) + "HSI2C/SPI buses" and "clocks" (:8-10) |

Under-fires on the repo's own idiom: **"what's the GSIV for uart2"**, "is D4c
done?", "close P2" — `GSIV`, milestone IDs and task IDs appear nowhere in the
description. Those prompts are handled by `progress.md`/HANDOFF, not by the
skill, so this is minor; but adding "GSIV/DSDT/ODIN" once would help.

**False positives — this is a global skill (`~/.config/opencode/skills/`), so it
is advertised in every project.** The trigger list at :12-13 is unqualified:
"research X", "find the constants for X", "what drives X", "port X to Windows",
"write/build a driver for X". In an unrelated repo those match: *"write a driver
for our PCIe IMU on Windows ARM64"*, *"what drives the touch panel on the
Pixel 6"*, *"find the constants for the i2c-designware clock"*. The scoping that
does exist ("Samsung/Exynos phones", "Use for any Exynos/Samsung porting
question", "Current project: Exynos 9820, Galaxy S10e") is prose at the start and
end, not a negative condition. The body's guard at SKILL.md:47-48 ("if the tree
is not an Exynos/WoA port, say so and ask") only runs **after** the skill has
been loaded and its ~11 kB consumed.
Fix: add to the description "Do **not** use for generic Windows/ARM64 driver
work on non-Exynos/Samsung hardware", and drop the bare "port X to Windows" /
"write/build a driver for X" phrases (they are the unscoped ones) in favour of
"port an Exynos subsystem to Windows".

**"Current project: Exynos 9820, Galaxy S10e" hard-codes a moving fact into a
global skill.** The same tree also carries the 9810/star2lte port
(`CONTRIBUTING.md`: the only validated device is SM-G965F). A 9810 session sees a
description telling it the project is the S10e. Better: "Primary target today:
Exynos 9820 / Galaxy S10e (`beyond0lte`); the 9810 / SM-G965F tree is the
validated base."

**Answer-only questions have no mode.** "what drives the touch panel" fires the
skill, and the skill's loop (§0-§6) then mandates a notes file, a task file,
milestone rows and one commit per step. There is no "just answer, cite the
existing notes" path, even though SKILL.md:45 says closed facts should not be
re-derived. Add a third mode: *answer* — read `notes/`, cite, commit nothing.

---

## 8. Two more checks, one of them against myself

**(H1) The skill's "calibration" findings are all real.** I verified every
example in `SKILL.md:162-164` and `review-checklist.md:61-74` against the repo:

| Skill claim | Repo evidence |
|---|---|
| 9810 ALIVE GPIO base carried into the 9820 touch port | `DRIVERS-MILESTONES.md:158` "Fix: Beyond0 touch port carried 9810 ALIVE-GPIO constants (base 0x14050000, GPA1_CON 0x040)"; `drivers/S6SY761TouchBeyond0/Device.h:40` |
| "register bits at 29/30 instead of 0/1" | `DRIVERS-MILESTONES.md:67` "FIFO enables bits 0/1 not 29/30, USI_RESET bit0 not 13, FS timing 0xF0/0xE0 not 0x0F/0x1F" |
| "a debounce of 0 ms on a key" | commit `f7d535f` "D8 follow-up: BTNS GpioInt DebounceTimeout 0 -> 10 ms (0x2710 x 100us)" |
| "five wrong source-line citations" | commit `1504fd4` "M12 constants review: fix five QCH source-line citations (values unchanged)" |
| "skip FORCE_VBUSVALID" inference contradicted | `DRIVERS-MILESTONES.md:68` |
| GCC `maybe-uninitialized` ×2 | `DRIVERS-MILESTONES.md:72` |
| `UfsDiag` ladder target inside USERDATA | `notes/m8-ufs-fence.md:95-97` "`UFS_SCRATCH_LBA` is still 15 614 336, a 9810 LBA that on THIS disk falls inside USERDATA (2 151 168..31 234 815) — the diag write ladder would stamp the middle of the Windows volume"; fixed in commit `3d6b04b` (`HANDOFF.md:379`) |

This is the part of the skill with the best return on tokens: it is specific,
sourced, and it is the reason the review step is worth running.

**(H2) "Each numbered step is its own commit" (SKILL.md:52) conflicts with the
repo's unit of work and with the skill's own §4.** HANDOFF §2 rule 2 and repo
AGENTS.md §3 say **one commit per *task*** ("never accumulate two tasks in one
commit"). A *step* is coarser: §4 ("Implement") covers the EDK2 module, the DSDT
device and the Windows driver package, which the D4c arc landed as three separate
commits (`28e85fd` DSDT, `9ec78e5` DSDT review, `6f445d4` skeleton).
`docs-and-commits.md:47` hedges by writing "One commit per task/step", i.e. the
skill uses two different units in two files. Read literally, SKILL.md:52 licenses
batching a whole implement step into one commit — the exact thing the repo
forbids. Fix: say "one commit per task; a step is usually several tasks", and use
the D4c ladder as the worked example.

---

## 9. Ranked findings

Every item gives the exact file+line to change and the fix. I did not edit the
skill. Paths are under `~/.config/opencode/skills/research/` unless noted.

### BLOCKER

**B1. `references/windows-driver-shapes.md:33-34` — the worked ASL example
contains an invented constant, and contradicts line 48 of the same file.**
`0x13EC0000, 0x1000` is `EXYNOS_PCIE0_IA_BASE` (the I/A microprogram sequencer,
`Exynos9820.h:452`) — a window the design deliberately keeps **out** of `_CRS`.
Real device: `Dsdt.asl:348-349` = `0x14FFE000, 0x00002000` (CFG0) and
`0x14000000, 0x00FF0000` (MEM).
Fix: replace lines 28-38 with the verbatim `Device (WLAN)` block from
`Dsdt.asl:341-357` (including `_UID`, `_CCA`, the `_CRS`-order comment and
`_STA`), and cite `firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl:343-357`.

### MAJOR

**M1. Commit prefix `research:` does not exist in this repo (0 of 298 commits).**
`SKILL.md:181-188`; `references/docs-and-commits.md:25-43` (and the grep at :28,
the review-fix form at :41); `references/review-checklist.md:6`;
`scripts/scaffold.py:89`; `commands/research.md:6-7`.
Fix: adopt the repo's form — `<ID> <area>: <what this commit establishes>` — and
use the real D4c ladder as the example (`D4c/BT research:`, `D4c research review:`,
`D4c constants:`, `D4c design:`, `D4c DSDT:`, `D4c DSDT review:`,
`D4c Windows half:`). Review fixes become `<ID> <area> review:` (cf. `M12
constants review:`, `M12 driver review:`). Change the grep to
`git log --grep='^D4c' --oneline`. If the user wants a `research:` tag, say so
explicitly as a *new* convention and note that it does not match history.

**M2. The repo mandates a research subagent; the skill's loop never mentions it.**
`SKILL.md:77-95` (step 1) vs `docs/exynos9820-port/DRIVERS-MILESTONES.md:172-173`
and `~/.config/opencode/AGENTS.md` ("Subagents").
Fix: add to step 1 — "for a subsystem not yet in `notes/`, delegate the evidence
pass to a subagent whose prompt names the notes file it must append to; the
implementation pass reads only that file."

**M3. `references/evidence-sources.md:124-135` — the dead-end list has no dates,
no establishing citation, and no reopening rule; one entry is already stale.**
The repo's costliest recent correction was re-walking such a verdict (commit
`aaff4b6`, `DRIVERS-MILESTONES.md:24`), and the GPU bullet (:126-128) predates the
D5b reopening (`DRIVERS-MILESTONES.md:54`, `notes/d5-gpu-display.md:321`).
Fix: per bullet append `(established by notes/<x>.md §N, <date>)` and
`reopens if <specific read>`; add one line: "a dead end negates a specific
question, not a subsystem — restate the claim before refusing to look."

**M4. ID selection is never explained; three namespaces exist.**
`SKILL.md:44`, `references/docs-and-commits.md:12-14`, `scripts/scaffold.py:51`
(`--id` help says "milestone/task ID, e.g. D9"). Reality: M1..M15 in
`MILESTONES.md`, D1..D8 (+D4b/D4c) in `DRIVERS-MILESTONES.md` (which at :4-5
"expands the M10–M15 rows"), and `tasks/` files use T/W/P.
Fix: add a 4-line "IDs" block to `docs-and-commits.md`: which file owns which
prefix, that D-rows expand M-rows, that task files take their own letter (T boot
path, W wifi, P parked — extend by subsystem letter), and that `scaffold.py
--id` should be given the **task** ID, with the milestone row pasted separately.

**M5. No branch for "the phone is not attached".** `SKILL.md:77-95`;
`references/evidence-sources.md:38` asserts "there is exactly one S10e here"
while `HANDOFF §24`/`§25` record `adb devices` empty.
Fix: first bullet of step 1 — "run `adb devices` (path depends on the host; see
M6). If empty: do not guess around it — record the exact read as a `needs-phone`
task with the date, and continue with dumps/mirrors."

**M6. `references/evidence-sources.md:37-38` — adb instructions are
Windows-only and stale on the box that runs this repo.**
`C:\Users\ilya\code\platform-tools\adb.exe` "invoked with `&`"; measured here:
`which adb` → `/usr/bin/adb`, on PATH, Linux.
Fix: "the adb path is host-specific — check `which adb` / `where adb` first; the
Windows path above applies only from the Windows box."

**M7. `SKILL.md:199` "Never push" with no branch policy.**
Measured: `HEAD` == `origin/drivers` == `6cdf8de`, `origin/drivers..HEAD` = 0 —
the branch is published every session; `AGENTS.md` §10 plans a history rewrite on
`main` after the drivers→main merge.
Fix: replace with "work on the branch the plan of record is on (`git status -sb`);
do not switch to or rewrite `main` (see `AGENTS.md` §10); push only when the repo
rules or the user say so."

**M8. The task-file template is a format no task file in the repo uses, while
`docs-and-commits.md:4-5` demands exactness.**
`assets/task-template.md:7,13,21,27,32` (`## Do this`, `## What it proves`,
`## Rollback`, `## Consent / safety`, `## Notes`) → 0 matches in `tasks/*.md`.
Real: `## Goal` (12 files), `## Done when` (13), `## Next actions` (9),
`## Known` (4).
Fix: rename the template's headings to Goal / Do this / What it proves / Done
when / Rollback-or-Do-NOT, and keep `Status: open — **needs-…**` (that line does
match `tasks/W01:3`, `tasks/W02:3`).

**M9. `scripts/scaffold.py` — four defects, all reproduced in
`/tmp/opencode/scaffold-test2`.**
- `:61` `subs` fills only `{Subsystem, ID, date, topic}`, so
  `assets/design-template.md:1` renders `# D9 — <subsystem> on the <device>` even
  with `--title` supplied; `:57` renders `tasks/D9-<slug>.md` though the slug is
  known. Fix: add `subsystem`, `device`, `slug` to `subs` (or rename the
  template's `<subsystem>`→`<Subsystem>`).
- `:58` `--repo` skips the `find_repo` plan-of-record check (`:22-28`), so notes
  and tasks were written into a non-port tree. Fix: validate `--repo` too.
- `:73-74` `--design` on such a tree raises an uncaught `FileNotFoundError`
  (exit 1) *after* writing notes+tasks — half-scaffolded topic. Fix: guard, or
  resolve the docs dir once before any write.
- `:68` `--id` is interpolated unsanitised: `--id ../../ESCAPE` wrote a file
  outside the repo. Fix: apply the same `re.sub` used for `slug`.

**M10. `SKILL.md:170` "one task file per rung" is heavier than the repo's
practice.** M12/D4b: 5 rungs → 2 task files (W01, W02); D4c: 6 rungs → 0 task
files, rungs live in `D4c-BT-DESIGN.md` + milestone rows.
Fix: "one task file per **device-visible action the tester performs** (a read, a
flash, a build), not per rung; rungs live in the design doc."

### MINOR

**m1. `references/evidence-sources.md:19`** — `vendor.raw` is at
`dumps/hhxj6/vendor/vendor.raw`, not `dumps/hhxj6/vendor.raw`
(cf. `notes/bt-driver.md:209`). Fix the path.

**m2. `:20`** — "the defconfig" is listed as a dump artefact; there is none
(`find dumps -iname '*defconfig*'` → empty; the shipped `kernel` has no IKCONFIG).
The HCI finding came from the mirror's
`arch/arm64/configs/exynos9820-beyond0lte_defconfig` (`notes/bt-driver.md:68,211`).
Fix: move the row to §3 and name the mirror path.

**m3. `:21`** — "PIT / GPT": only a PIT exists locally; GPT geometry came from
device reads (`notes/s10e-partitions.md`, `notes/pit-geometry.md`).

**m4. `scripts/scaffold.py:85`** — the printed milestone row is 3 cells with an
unbolded `todo`; the Milestones table is 4 columns (`DRIVERS-MILESTONES.md:46`)
and states are bolded. Fix the printed row to
`| **todo** | <title> | — |` for the milestone table, or label it as a task-table row.

**m5. `SKILL.md:68,217`** — `scripts/scaffold.py` is cited as a relative path;
from the repo cwd it does not exist (`ls scripts` → none). Fix: give
`~/.config/opencode/skills/research/scripts/scaffold.py`.

**m6. `SKILL.md:216` vs the loop** — `assets/driver-readme-template.md` and
`references/docs-and-commits.md` are never pointed at from the step that needs
them (step 4 and steps 0/6). Fix: add the pointers in-line.

**m7. `assets/notes-template.md:24,32,43`** — `## Inference`, `## Constants
frozen`, `## Review log` appear in 0 of 51 notes files; the exemplar
`notes/bt-driver.md` puts the review log and Sources as h3 inside the progress
log (`:178,:192,:206`). Fix: match the exemplar, or state that the template is
new and intended to become the convention.

**m8. `references/evidence-sources.md:144`** — `MAP.md` is cited without noting
it is stale and 9810-centric ("Generated 2026-10-08 … SM-G965F (Exynos 9810)",
`MAP.md:1-8`), which contradicts `SKILL.md:14`.

**m9. `SKILL.md:140-141`** — the Linux verification build is mandated but
`notes/d1-build-env.md` (edk2 clone, required submodules, no-sudo constraints, the
ephemeral `/tmp/opencode/build-uefi-linux.sh`) is never cited. Add the pointer.

**m10. `SKILL.md:204-206`** — the leakage hard stop has no gate; `CONTRIBUTING.md`
and `tools/release/scan.py` exist and CI runs them. Add: "run
`python tools/release/scan.py` before the arc's last commit."

**m11. `references/docs-and-commits.md:81`** — grepping the built `Dsdt.aml` /
`FVMAIN.Fv` cannot be re-run later: `.gitignore` has `Build/`, `edk2/`, `*.fd`
and `git ls-files firmware/edk2` is empty. Fix: "record the command **and the
matching output line** in the notes (as `notes/bt-driver.md:200` does)."

**m12. `references/windows-driver-shapes.md:25,87`** — the taxonomy omits the
root-enumerated shape the repo chose for audio (`ROOT\SimpleAudioSample`,
`notes/d8-audio-windows.md:32`; portcls skeleton at `DRIVERS-MILESTONES.md:164`).
`SimpleAudioSample` appears nowhere in the skill. Add a row and name the sample.

**m13. Duplication = drift.** `SKILL.md:147-152` ≈ `windows-driver-shapes.md:92-104`;
`SKILL.md:154-164` ≈ `review-checklist.md:1-7`; `SKILL.md:179-192` ≈
`docs-and-commits.md:23-54`; `SKILL.md:194-206` re-states HANDOFF §2 / AGENTS.md;
`SKILL.md:63-66` ≈ `commands/research.md:11-14`. Fix: one owner per rule, the
other file points at it.

**m14. `references/evidence-sources.md:137-144`** — "Sources consulted while
writing this skill" is build metadata for the skill, not usable evidence, and
contains a glob (`notes/d8-audio*.md`). Delete or move to a changelog.

**m15. `SKILL.md:2,16`** — `metadata.opencode/autoinvoke: true` is a no-op (only
`false` has an effect); `name: Exynos WoA research` breaks V1/Claude-Code
name rules (folder is `research`, V1 wants `^[a-z0-9]+(-[a-z0-9]+)*$` and
name==folder). Drop the key or set it deliberately; keep the label but know it is
V2-only.

**m16. `SKILL.md:12-13`** — the trigger phrases "port X to Windows" and
"write/build a driver for X" are unscoped in a **global** skill; add "do not use
for non-Exynos/Samsung Windows driver work" to the description and scope those
two phrases.

**m17. `SKILL.md:63-66`** — no answer-only mode. "what drives the touch panel"
should be answerable from `notes/d8-audio.md` with zero commits, but the loop
mandates a scaffold + 7 commits. Add mode 0: *answer* — cite existing notes,
commit nothing.

**m18. `SKILL.md:52` vs `docs-and-commits.md:47`** — "each numbered step is its
own commit" vs "one commit per task/step" vs repo "one commit per task". A step
is several tasks (D4c's implement step = 3 commits). Fix: state the task as the
unit.

**m19. `references/docs-and-commits.md:60-69`** — the state vocabulary is
narrower than practice: the repo also writes `**design done**`,
`**done — decision**`, `**todo — one safe read**`,
`**done (built into the FD; Windows enumeration unvalidated)**`
(`DRIVERS-MILESTONES.md:108,147,164`). Add the qualified forms or say
qualifications are free text after the bolded state.

---

## 10. Verdict

The skeleton of the skill is right and unusually well-grounded: the loop, the
VERIFIED/UNVERIFIED/DECISION split, the rung ladder, the two-halves contract, the
build-evidence table and the calibration findings all match the repo, and I could
verify nearly every hardware claim mechanically (GSIVs 285/408, `_HID` values,
the 9810 bases, the dead ends, the MS-PL licences).

What is wrong is concentrated in three places: (1) the one code block an agent
will copy is factually wrong and self-contradicting (B1); (2) the process rules
that touch git — commit prefix, push/branch policy, task-file IDs and formats —
were written from the skill's own model rather than from the 298 commits and 15
task files in the tree, so they will actively fragment the plan of record
(M1, M4, M7, M8, M10); (3) the loop has no failure branches — no phone, no WDK,
no prior art, dead end, already-researched — which are the common cases in this
repo (M3, M5, m9).

Cheapest high-value fixes, in order: B1, M1, M4, M3, M9, M5/M6.

## Sources consulted in this review

- `~/.config/opencode/skills/research/**` (all 10 files, read in full)
- `~/.config/opencode/commands/research.md`
- repo: `AGENTS.md`, `CONTRIBUTING.md`, `MAP.md`, `progress.md`,
  `docs/exynos9820-port/HANDOFF.md` §2/§24/§25,
  `docs/exynos9820-port/MILESTONES.md`, `docs/exynos9820-port/DRIVERS-MILESTONES.md`,
  `firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl`,
  `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`,
  `notes/{bt-driver,m12-wifi-chip,d1-build-env,d1b-usb-vbus-hcd,d4-usb-wifi,d5-gpu-display,d8-audio-windows,d8-audio-linux,s10e-live-reads,m8-dscfdf,m8-libs,git-identity}.md`,
  `tasks/{W01-wifi-r1-linkup,W02-wifi-live-reads,T07-first-boot,P03-cmu-bank-hazard}.md`,
  `drivers/Exynos9820Bt/*`, `drivers/Exynos9820Wlan/*`, `drivers/S6SY761Touch*/…`,
  `installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs`, `.gitignore`
- `git log` (298 commits), `git rev-parse`, `git ls-files`, `git status -sb`
- GitHub license API: `microsoft/Windows-driver-samples` and
  `worproject/cywbtserialbus` → both `spdx_id: MS-PL`
- OpenCode skills docs (`opencode.ai/v2/docs/skills/`, `opencode.ai/docs/skills/`)
  for `metadata.opencode/autoinvoke` and the V1 name/description rules
- scaffold test tree: `/tmp/opencode/scaffold-test2/` (the real repo was not
  written to by the scaffold; only this notes file was added)

## 11. Self-verification of this review

- Every `file:line` citation in this file was re-checked mechanically: **169
  unique citations, 169 resolve to an existing file, 169 within the file's line
  range** (script over the notes file; roots = repo + `docs/exynos9820-port` +
  the skill's `references/`, `assets/`, `scripts/` + `commands/`).
- Two citation errors I made and corrected in place: `notes/bt-driver.md:189`
  → `:200` (the `Dsdt.aml` byte grep), `:205` → `:209`, `:208` → `:210`;
  `DRIVERS-MILESTONES.md:159` → `:158`, `:145` → `:147`, `:157` → `:164`
  (SimpleAudioSample row); the `UfsDiag` USERDATA quote is
  `notes/m8-ufs-fence.md:95-97`, not a milestone row.
- One substantive error I made and corrected: §2(g) first draft claimed
  `grep -rn Armada` and `grep -rn ExynosAuto` found nothing. They do — the
  greps had not been run. §2(g) now records the verified results. This is the
  exact defect `review-checklist.md:21-23` warns about, so it is worth stating
  plainly rather than quietly fixing.
- The real repo was never scaffolded into: `scaffold.py` was run only against
  `/tmp/opencode/scaffold-test2/`. The only file added to the repo by this
  session is `notes/skill-research-review.md` (untracked, not committed — the
  brief was to produce the review, not to commit it).
