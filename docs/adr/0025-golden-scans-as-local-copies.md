# 0025. Golden scans as local, hash-checked copies

- Status: Accepted
- Date: 2026-10-16

## Context
Section 9 planned the golden scans in Git LFS. The M1 golden set is four Revopoint scans of 14 to 67 MB each (OBJ), made by the project owner, and later sets will be larger. LFS would put them in every clone and use the hosting quota, and the originals live on the owner's machine anyway. The limits in each `metrics.json` only mean something for the exact file they were measured on.

## Decision
Each `tests/golden/<name>/` folder holds the session and the limits in Git, and a local copy of the scan named `scan.<ext>` that `.gitignore` excludes. `metrics.json` records the source path, size and SHA-256 of the file the limits were set on, and the golden test fails if the copy differs. A folder without its scan is skipped with a warning, so CI (which has no scans) reports the test as skipped. `M2C_GOLDEN_DIR` can point at a golden set kept elsewhere.

## Alternatives considered
- Git LFS: scans travel with the repository, but every clone downloads them and the quota fills quickly.
- Reading the originals in place: no copy, but nothing proves which file the limits belong to, and a moved or edited original silently changes the test.

## Consequences
- The golden run is a manual step on a machine with the scans, before each milestone gate; nightly CI cannot run it.
- A second developer needs the scans copied by hand; the SHA-256 tells them whether they have the right files.
- Changing a scan means new limits in a separate, reviewed commit (AI guidelines rule 10).
