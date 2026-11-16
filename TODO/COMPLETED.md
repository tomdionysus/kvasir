# Kvasir - Completed

The record, newest first. Each entry says what landed, with its gate counts.

## 2026-11-16: collations from Unicode's table, and string comparison

- Landed (ruling 32): Unicode's allkeys.txt for UCA 9.0.0 with its license;
  `kvasir-ducet`, which compiles it at build time; the collation algorithm
  by hand for utf8mb4_0900_ai_ci, _as_cs, utf8mb4_bin, utf8mb4_0900_bin and
  binary; string comparison in the evaluator under the collation MySQL
  derives for the pair. Nothing of MySQL copied: every expected sort key
  and answer was observed on the reference server.
- Found on the reference server: MySQL's 0900 collations weigh each code
  point alone, applying none of the table's 868 contractions (L with a
  middle dot, Cyrillic short I, Thai and Lao prevowels, Tibetan and Tamil
  sequences all checked). A first implementation that applied them, and a
  second that reordered prevowels, each failed on observed keys and were
  dropped.
- Found by the gates, each fixed with a test: a binary string against text
  (X'61' = 'a', and every comparison under SET NAMES binary) was compared as
  numbers, giving wrong answers silently; the parser refused SET NAMES
  binary, since BINARY is a reserved word.
- Test-first: the collation suite failed against a stub (the stub run
  crashed on a null collation, exit 139, so its count was not seen); the
  comparison tests and the differential gate's twelve new statements failed
  first (4 failed, the gate 113 of 121).
- Gate: release on node-c and node-b **262 passed, 0
  failed**, the differential gate **121 of 121**. Hosted CI on 2f13512: see
  the next entry or its run.

## 2026-11-16: the connection, the differential gate, the first fuzz runs

- Landed: the client connection over a transport seam; the recorded
  differential gate (109 of 109 statements as MySQL 8.4 answers them); the
  diagnostics area and SHOW WARNINGS / ERRORS; the session functions; the
  node's settings and `kvasir-node hash-password`.
- Found and fixed by the gates, each with a test: the gate's first run
  found two type differences (an unset variable read as a hex literal in
  arithmetic; strings under a _bin collation flagged BINARY); hosted CI's
  LeakSanitizer found that a connection whose client left mid-read kept
  itself alive through its own continuation (it would have leaked every
  such connection in the node); the lexer's first fuzz run found an empty
  token after "name..". The recorder's own marker queries were found to
  clear the diagnostics it was recording, and now mark from the client.
- Test-first: the connection 10 of 10, the settings 7 of 7, the session's
  diagnostics and functions 3 of 3 failed first; the parser's SHOW test was
  written after its code, and the leak test after the fix (the failing
  evidence is CI's report).
- Gate: release on node-a **254 passed, 0 failed**, check passed;
  each fuzz target a minute clean after the fix (lexer 3.2 million inputs).
  Hosted CI on the fixes: see the next entry or the run for 5755e42.
- Mistake made and undone: a clang-format pass was run on the fuzz
  CMakeLists.txt and mangled it; restored from git before commit.

## 2026-11-16: step 2a, the evaluator, the session and the command phase

- The MySQL 8.4.13 reference server (ruling 30) was set up on node-b
  and used as the oracle: most expected types, lengths, flags, values,
  warnings and errors in the new tests were observed on it, and the
  greeting encoder was checked against its bytes.
- Landed: exact decimals and MySQL's text for doubles; typing and
  evaluating constant expressions; the system variables; the session; the
  command phase. Each suite failed against stubs first (14 of 14 decimal, 15
  of 15 expression, 11 of 11 variables; the session's and the command
  phase's stub runs crashed on an empty result, so their counts were not
  seen), then passed.
- Hosted CI ran again once the repository was public, and its x86-64 ASan
  and TSan found a use-after-free in the parser (an argument read from a
  token that `take()` had freed; GCC's order differs on arm64) that the
  Pi's ASan run passed. Fixed and recorded; hosted CI is now part of the
  gate.
- The Pis moved to a 4K-page, 39-bit kernel: TSan works there, ASan does
  not start (g++ or clang). An ASan build on 4 GB node-d was among
  tonight's out-of-memory hangs there; Kvasir moved off it at Lir
  coordinating session's request.
- Gate: release on node-a **230 passed, 0 failed**; check passed;
  hosted CI on the evaluator and variables commits passed every job
  (release x86-64 and arm64, clang, ASan, TSan, coverage, CodeQL). The
  session commit failed hosted `check` (formatted on a Pi without
  clang-format); the next commits carry the formatting.
- Next: the connection on Dannan's listener, then `kvasir-node` on 3306
  (needs Dannan 0.8.4's `start_tls_unanswered`), then the differential gate
  as a tool. No claim is held but the light one for the container on
  node-b.

## 2026-11-15: step 2a, second session: authentication and the SQL front

- Checked the plan against the tree and the siblings. Found that Dannan's
  `Stream::start_tls` refuses MySQL's upgrade (it ends the connection when
  the ClientHello is already waiting); Dannan agreed
  `start_tls_unanswered`, proposed to the maintainer for 0.8.4. Asked Dannan to export
  its fuzz harness (taken as a 0.9 item). Gave CodeQL `actions: read`.
- `caching_sha2_password` (known answer computed with the openssl command
  line on a Pi), the sign-in state machine and verifier cache, the built-in
  accounts (Argon2id through Dannan), the lexer, the reserved words (262,
  read from the 8.4 manual), the syntax tree and the parser for the first
  statements; fuzz targets for the lexer and parser.
- Test-first held for every piece: each suite failed against stubs (10 of
  14 sha2, the other four passing on the stubs' constants and empty
  answers; 23 of 25 sign-in; 10 of 10
  accounts, 37 of 38 lexer, 31 of 31 parser), then passed. Three test bugs
  were found on the way and corrected to the stated contract (a bound off
  by one, iterators from two temporaries, a 15 that was 14).
- Gate on node-d (GCC 14, warnings as errors, `-j2`, nice 19):
  `tools/ci.sh release` **163 passed, 0 failed**; `tools/ci.sh asan`
  **163 passed**; `tools/ci.sh check` passed. Every fuzz target compiles
  under g++ `-fsyntax-only`. TSan cannot run on the Pis (VMA 47); clang
  runs nowhere.
- Hosted CI stopped: GitHub refuses every job for the account's billing.
  After the maintainer made the repository public, hosted CI ran again, and its x86-64
  ASan and TSan jobs found a heap-use-after-free in the parser that the Pi's
  ASan run had passed. The parser called `node(kind, t.offset,
  take().value)` with `t` a reference to the token `take()` frees; argument
  order is unspecified, and GCC evaluates it differently on x86-64 and
  arm64. Fixed by copying the offset first, in `primary()` and
  `limit_value()`. The lesson: a sanitizer run on one architecture is not
  the gate; hosted CI's x86-64 runs are part of it.
- No claim is held. Resume from `TODO/ACTIVE.md`, "Resume here".

## 2026-11-15: handoff at the end of the first session

- The documents were brought into line for a session starting cold: the
  design carries rulings 1 to 31 in its body; the reconciliation document
  was folded into it and removed (it is in the git history); the review is
  kept as a record; the level standard was rewritten clean; the README's
  status says what is built.
- State: `develop` is pushed to the private remote. Last local gate:
  `tools/ci.sh release` 44 passed and `check` passed, on node-d. No
  claim is held on any build machine.
- Resume from `TODO/ACTIVE.md`, "Resume here".

## 2026-11-15: step 2a, first piece: the wire protocol's codec

- `kvasir/protocol/codec` (MySQL's little-endian integers, length-encoded
  integers and three string forms; Dannan's codec is big-endian, so this is
  Kvasir's, on Dannan's `Bytes`), `packet` (framing, 16 MiB fragments,
  bounded on each header, sequence checked) and `messages` (HandshakeV10,
  HandshakeResponse41 and SSLRequest with every field bounded, OK, ERR, EOF,
  the authentication packets, column definitions and text rows).
- Tests from the MySQL 8.4 manual, with its example bytes for COM_QUIT, OK,
  ERR and EOF as recalled; not yet checked against a MySQL server.
- Test-first held for `messages`: 21 cases failed against stubs, then
  passed. It did not hold for `codec` and `packet`, whose code was written
  before their tests.
- Gate on node-d (GCC 14, warnings as errors, `-j2`, nice 19):
  `tools/ci.sh release` **44 passed, 0 failed**; `tools/ci.sh check` passed
  (header block and clang-format), its first run anywhere.
- Not run: clang, ASan, TSan, coverage. The fuzz target
  (`kvasir/fuzz/packet_fuzz.cpp`) has never been compiled: no build Pi tried
  has clang.
- Next in 2a: `caching_sha2_password`, the connection (greeting, TLS
  upgrade, sign-in, commands) on Dannan's listener, the lexer and the first
  statements, `kvasir-node` on port 3306.

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
