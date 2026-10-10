# Git commit identity — incident and fix (2026-10-11)

## What was found

- Every commit in this repo (all 260, back to the 2026-09-27 "Initial public
  release") was authored/committed as
  `ntdevlabs <ntdevlabs@users.noreply.github.com>`.
- The user is **not** ntdev. That email is the GitHub noreply address of the
  `ntdevlabs` account, so GitHub rendered all commits under someone else's
  identity on `ilyatbn/exynos-woa`.
- Cause: the repo-local `.git/config` carries a `[user]` block with that
  identity, and this machine has **no global git identity** — so every
  `git commit` (all of them from agent sessions) silently inherited it.
- Origin of the block: unprovable. It predates this clone (the clone's own
  reflog entry already carries the ident, and the remote's first commit was
  ntdevlabs too). Likely set when the repo was first published — the tree
  continues `ntdevlabs/exynos9810-woa` — possibly by a GUI tool; GitKraken
  touched this repo (`.git/gk/config` exists) and writes the logged-in
  account's identity into `.git/config`. No repo doc ever instructed
  commits under that name.

## What was done

- Repo-local identity set: `user.name=ilyatbn`,
  `user.email=ilyatbn@users.noreply.github.com`.
- `git filter-branch --env-filter` rewrote the **29 drivers-only commits**
  (`main..drivers`, d60bbb0..272b491) to the new identity.
  Verified mechanically: `git log --format='%an|%cn' main..drivers | sort -u`
  shows only ilyatbn; `git diff 272b491 HEAD` is empty (content identical);
  `main` untouched at a8d8a62.
- Force-pushed: `origin/drivers` 272b491 -> **419fc03** (new tip).
- Backup ref kept: `refs/original/refs/heads/drivers` (old tip 272b491);
  delete with `git update-ref -d refs/original/refs/heads/drivers` once happy.

## Still open

- **main**: ~230 ntdevlabs commits remain there **by user decision** —
  rewrite after merging drivers into main ("later we will merge this to main
  then rewrite the rest").
- **Windows clone** `C:\Users\ilya\code\exynos-woa`: its `.git/config` likely
  has the same ntdevlabs `[user]` block — check and fix; its `drivers` branch
  must be reset to the rewritten `origin/drivers`
  (`git fetch && git reset --hard origin/drivers` — only if no unpushed local work).
- After the main rewrite, force-push main; anyone else with clones re-fetches.

## Rule for future sessions

Check `git config user.name user.email` before the first commit of a session.
Never commit as ntdevlabs. (Now also in AGENTS.md §10.)
