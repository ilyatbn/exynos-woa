# Agent rules — this repo

These stack on top of the global rules and the ground rules in
[`docs/exynos9820-port/HANDOFF.md`](docs/exynos9820-port/HANDOFF.md) §2 and
[`docs/exynos9820-port/MILESTONES.md`](docs/exynos9820-port/MILESTONES.md).
If a rule here appears to relax one of those, the repo rule wins.

## 1. Think long, act small

The output budget is large (131k tokens). Use it for reasoning, **not** for
one giant burst of work. A long single response is unrecoverable if it dies;
a chain of small commits is not.

## 2. Persist findings immediately, never at the end

- Every search, read, build, test or grep result is written to a `notes/*.md`
  file **as soon as it is known**, before moving to the next step.
- One notes file per task, with a running **Progress log** section that is
  appended to (current pattern: `notes/m8-ufs-hc.md`, `notes/m8-build.md`).
- Do not hold results only in memory or only in the chat response. If the
  session crashes, the notes file must still contain everything found so far.
- Prefer many small writes over one large write. Checkpoint after every
  meaningful step.

## 3. Chunk tasks smaller than the work

- If a step turns out to need more than one coherent change, split it into
  separate tasks **before** doing them, and add them to the todo list.
- One git commit per task, per the repo's ground rule. **Commit between tasks**
  — never accumulate two tasks in one commit, never batch commits.
- **Collapse at the end of an arc** (global rule): those per-task commits are the
  recovery net while working; once the arc is finished and verified, squash it
  into one commit (`git reset --soft <arc-base>` + one commit, tagged
  `<arc>-pre-squash` first). Then repair any commit-hash references the rewrite
  invalidated. Never collapse in-flight or unrelated history.
- **No user attributions in committed files** (global rule): no quotes, no
  "user directive …", no "per the user", no "the owner's call". Record decisions
  as neutral facts/policy with the date kept. Scrub such lines whenever a file
  is touched.
- Move the `MILESTONES.md` line (`todo → doing → done`) in the **same commit**
  as the work it describes.
- A task is not `done` until its verification actually ran.

## 4. Review every commit as a harsh critic

- After **each** commit, re-read the diff of that commit and review it as if you
  were an adversarial code reviewer: assume it is wrong until proven otherwise.
- Attack the substance, not the style — a constant that was guessed, a rename
  that missed a call site, a guard that no longer compiles out what it claims,
  a comment that asserts something the code does not do, dead code left behind
  a `#if 0`, an off-by-one, a silent behaviour change.
- Verify the claims mechanically (`rg`, `git diff --stat` on the 9810 tree, a
  build), never by re-reading them.
- Anything the review flags becomes a **follow-up task**, fixed in its own
  commit — never folded into the commit under review, never left as a note.
- Record the review in the task's notes file: what was checked, what was
  found, what was fixed.

## 5. Restart points

- `HANDOFF.md` gets a new numbered section per session crash: what is
  committed, what was in flight, and the exact next order of work.
- Update it **before** the commit that ends the session's last task, so a
  fresh session can resume from `git status` alone.

## 6. Hardware constants

- Never guess a constant. Unverified values stay named `UNVERIFIED` in the
  source, with the read that would close them recorded in the notes file.
- Verified MMIO addresses come only from `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`.
- The 9810 packages stay **byte-unchanged**; port by copying into the
  `Beyond0Lte*` paths, never by editing the 9810 tree.

## 7. Nothing is `Ready` without device validation

Unvalidated stages stay `Guided` / `NotImplemented`. A successful build says
nothing about a boot hazard; record the difference.

## 8. The S10e is connected — `adb` is not on PATH

`adb` lives at `C:\Users\ilya\code\platform-tools\adb.exe` (call it with `&`,
it is not on `PATH`). The phone is attached and authorized:

```
& "C:\Users\ilya\code\platform-tools\adb.exe" devices   # RF8M21HYDSZ   device
```

Device: `RF8M21HYDSZ`, `model:SM_G970F` (Galaxy S10e, the **only** one).

- Reads are cheap and safe; use them before guessing a constant.
- What works: `/proc/partitions`, `/proc/cpuinfo`, `/proc/device-tree/<node>/`
  **names** and symlink targets. What does **not**: property contents
  (SELinux-blocked to an unprivileged shell), `/sys/**` deeper than one level.
- **Never run anything destructive** — no partition erasing, no flashing, no
  powered-dock/host-mode test — until Windows booting is established
  (HANDOFF §2 rules 5 and 6).

## 9. After a failed boot: read Samsung's own reset log before guessing

The screen during a boot loop shows almost nothing usable (tiny red/blue/yellow text for
< 1 s). Samsung records every reset; read it from TWRP instead:

```
adb shell 'cat /proc/debug_history'          # one line per boot: time, reason, INFORM3, boot path
adb shell 'cat /proc/reset_reason; cat /proc/reset_reason_extra_info'
adb shell 'cat /proc/cmdline' | tr ' ' '\n' | grep -i reset
```

- Go **straight from the loop into TWRP** (Vol Up + Bixby + Power). Passing through download
  mode adds its own entries.
- `debug_history` is mostly NUL/space padding: strip it with `tr -c '[:print:]\n' '\n'`, keep
  lines starting with `[`.
- What the entries meant on 2026-10-10 (`notes/m8-uefi-image.md` B35,
  `notes/s10e-debug-history-20261010.txt`): `RP / SOFT, INFORM3(abc0ead7) > NORMAL` every ~15.5 s
  is a loop where sboot jumped to our image and never heard back (a fixed timer, not a crash).
  Android reboots show `REBOOTMODE0` / `INFORM3(0)`; kernel panics show `KP / SOFT, PANIC`.
  The **spacing between entries** is the useful number: same period across builds = timer.
- `reset_reason` itself only names the last reset (usually your key combo, `RPON`).

## 10. Commit identity — verify before the first commit

- Before the **first** commit of any session, check `git config user.name` and
  `git config user.email`. They must be `ilyatbn` / `ilyatbn@users.noreply.github.com`.
- Never commit as `ntdevlabs` — that was a stale repo-local identity that mis-attributed
  260 commits (incident + fix: `notes/git-identity.md`).
- `main` still carries ntdevlabs commits on purpose; they get rewritten after the
  drivers→main merge. Do not "fix" them early.
