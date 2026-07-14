# Runbook — purge non-distributable blobs from git history

`git rm --cached` (commit `7c939fa`) removed the copyrighted ROMs / media and personal user data from
**HEAD**, so the current tree, fresh checkouts and releases are clean. But those ~570 ROMs + DOS/Win
bundles + Wallace&Gromit media + private notes still live in **older commits** — a public clone still
downloads them. To make them absent from the repo *and* shrink it, history must be **rewritten**.

This is a **destructive, deliberate** operation: it changes every commit SHA after the earliest touched
commit, requires a **force-push**, and **breaks every existing clone and fork** (they must re-clone).
Do it in a maintenance window, announced, with a backup. It is intentionally NOT automated.

## 0. Back up first (non-negotiable)
```bash
git clone --mirror https://github.com/indecenti/NucleoOs.git NucleoOs-backup.git   # full, restorable
```

## 1. Install git-filter-repo (faster + safer than filter-branch/BFG for this)
```bash
pip install git-filter-repo          # or: brew install git-filter-repo
```

## 2. Paths to strip (same set that is now gitignored)
Create `purge-paths.txt`:
```
deploy/sd/data/ROMs/
deploy/sd/data/DOS/
deploy/sd/data/Documents/
deploy/sd/data/Notes/
deploy/sd/data/Recordings/
deploy/sd/data/Videos/
deploy/sd/data/ir/userdata.json
tools/sd-sim/data/ROMs/
tools/sd-sim/data/DOS/
tools/sd-sim/data/Documents/
tools/sd-sim/data/Notes/
tools/sd-sim/data/Recordings/
tools/sd-sim/data/Videos/
tools/sd-sim/data/ir/userdata.json
```
(Personal `*.txt`/`*.todo`/`*.md` at `deploy/sd/data/` and `tools/sd-sim/data/` root — add the exact
filenames from `git log --all --name-only | grep -E 'data/(diario|note|report|todo|tasks|welcome)'`.)

## 3. Rewrite history (fresh mirror, not your working clone)
```bash
git clone --mirror https://github.com/indecenti/NucleoOs.git NucleoOs.git
cd NucleoOs.git
git filter-repo --invert-paths --paths-from-file ../purge-paths.txt --path-glob 'deploy/sd/data/*.txt' --path-glob 'tools/sd-sim/data/*.txt'
git count-objects -vH        # confirm the repo shrank
```

## 4. Force-push — GOTCHA: the >2 GiB HTTPS wall
Even after purging, the pack can exceed GitHub's **2 GiB single-push limit over HTTPS** (which returns a
masked 500). Push over **SSH** and, if a single push still exceeds 2 GiB, push **incrementally** — walk
the rewritten history and push it in <2 GiB chunks so no single transfer trips the limit:
```bash
git remote set-url origin git@github.com:indecenti/NucleoOs.git    # SSH (deploy key)
# single shot if it fits:
git push --force --mirror origin
# else incremental (push older→newer in chunks):
for c in $(git rev-list --reverse HEAD | awk 'NR % 400 == 0'); do git push --force origin +$c:refs/heads/main; done
git push --force origin +HEAD:refs/heads/main
git push --force --tags origin
```
See memory `git-2gb-push-limit-incremental` for the SSH-deploy-key + staged-push detail.

## 5. After the push
- **Everyone re-clones.** Old clones/forks now diverge and must be discarded or rebased.
- Tell GitHub Support (optional) to purge cached views of the removed blobs.
- Verify: `git log --all --diff-filter=D --name-only | grep -i ROMs` returns nothing in a fresh clone,
  and `git rev-list --objects --all | grep -iE '\.(gg|gb)$'` is empty.

## Cheaper alternative if the force-push is too painful
Leave history as-is (HEAD is already clean) and, if it's ever a takedown concern, open a private
support request to GitHub to strip the specific blobs — no history rewrite, no force-push. The releases
published by `.github/workflows/release.yml` are built from the clean HEAD regardless.
