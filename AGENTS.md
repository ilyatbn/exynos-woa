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
- Move the `MILESTONES.md` line (`todo → doing → done`) in the **same commit**
  as the work it describes.
- A task is not `done` until its verification actually ran.

## 4. Restart points

- `HANDOFF.md` gets a new numbered section per session crash: what is
  committed, what was in flight, and the exact next order of work.
- Update it **before** the commit that ends the session's last task, so a
  fresh session can resume from `git status` alone.

## 5. Hardware constants

- Never guess a constant. Unverified values stay named `UNVERIFIED` in the
  source, with the read that would close them recorded in the notes file.
- Verified MMIO addresses come only from `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`.
- The 9810 packages stay **byte-unchanged**; port by copying into the
  `Beyond0Lte*` paths, never by editing the 9810 tree.

## 6. Nothing is `Ready` without device validation

Unvalidated stages stay `Guided` / `NotImplemented`. A successful build says
nothing about a boot hazard; record the difference.
