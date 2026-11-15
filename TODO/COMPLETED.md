# Kvasir - Completed

The record, newest first. Each entry says what landed, with its gate counts.

## 2026-11-15: the design, second draft

- The maintainer asked whether the first draft made sense and for every other session's
  advice. The session's review, eleven sessions' answers (Hero Graphics
  declined) and the reconciliation are in `2026-11-15-design-review.md` and
  `2026-11-15-rationalised-design.md`.
- The maintainer ruled on the levels, the sticky proposer, waiting writers, the shared
  simulator, the tagline, MySQL 8.4, commits, and the level names as a group
  standard (rulings 6 to 23). The design document was rewritten from them.
- Kvasir reviewed Dannan's simulator design and its reconfiguration
  proposal; Dannan took every point. The operator-declared root in Dannan's
  first draft was challenged by the maintainer and replaced by a slot table with no
  root except at genesis (relayed by Dannan as ruled).
- No code changed after the skeleton's gate. Handoff: `TODO/ACTIVE.md`.

## 2026-11-15: step 1, the skeleton

- The stack layout from Mimir's: CMake with Dannan pinned at 0.8.2, the
  `kvasir` library with its generated version header, `kvasir_tests`,
  `kvasir-node --version`, `tools/ci.sh`, the hosted CI set, the laws copied
  from Dannan's, `CLAUDE.md`, `CONTRIBUTING.md`, the README, and `git init`
  on `develop`. Nothing is committed and there is no remote.
- The design draft, `TODO/2026-11-15-kvasir-design.md`: rulings 1 to 6, the
  rest Proposed. The Mimir session answered on its acknowledgement levels;
  no session has reviewed the design.
- Gate: `tools/ci.sh release` on node-c (GCC 14, warnings as errors,
  `-j2`, nice 19): built clean, **1 passed, 0 failed**. `kvasir-node
  --version` prints `kvasir-node 0.1.0`; without it the node exits 2 saying
  it serves nothing yet.
- Not run: `tools/ci.sh check` (node-c has no clang-format), and the clang,
  sanitizer and coverage steps.
- Re-gated after the tagline became "The Distributed Relational Database"
  and the Dannan repository setting became a string: `tools/ci.sh release`
  on node-c, **1 passed, 0 failed**; the header block is on every
  source (checked by the `check` step's loop; clang-format not run, node-c
  has none). Committed on `develop` at the maintainer's ruling.
- Handoff: `TODO/ACTIVE.md`, "Waiting on the maintainer". The acknowledgement-level
  questions were put to the maintainer interactively and he wished to clarify them
  first; his comment did not reach the session.
