# Kvasir - Active Work

Last updated: 2026-11-16, written for a session starting with no memory of
it.

## Read, in this order

1. This file.
2. `TODO/2026-11-15-kvasir-design.md`: the design. The maintainer's rulings 1 to 31,
   the rulings other sessions relayed, and what is still proposed.
3. `TODO/2026-11-15-ack-levels-standard.md`: the four level names, a
   standard for the whole stack.
4. `TODO/COMPLETED.md`: what landed, with gate counts.
5. Mimir's design (`../mimir/TODO/2026-11-15-mimir-design.md`) and the
   stack document (`../dagda/TODO/2026-11-03-stack.md`), as `CLAUDE.md`
   asks, before work that touches the cluster or another project.
6. `TODO/2026-11-15-design-review.md` only when the sessions' measurements
   and warnings are wanted (the Pis' failures, Dagda's rates, the clocks).
   It reviews the first draft and is a record, not the design.

## Resume here

Step 2a, one node speaking the MySQL protocol, is in hand.

**Done and pushed** (268 tests; hosted CI's x86-64 ASan and TSan are the
sanitizer gate, since ASan cannot start on the Pis' 39-bit kernels):

- `kvasir/protocol/`: the value codec, packet framing, the connection
  messages, and `caching_sha2.h` (the scramble, the fast-path check, the
  full path's password packet).
- `kvasir/server/sign_in.h`: the connection phase as a state machine with
  no I/O (greeting, SSLRequest, TLS seam, response, auth switch, fast and
  full paths, MySQL's refusals), and `VerifierCache` (fast-path verifiers in
  secure memory, bounded). `accounts.h`: the built-in accounts, Argon2id
  credentials (the format is in the design, Proposed).
- `kvasir/sql/`: the lexer, MySQL 8.4's 262 reserved words, the syntax tree
  and the parser for SELECT (select list, aliases, FROM DUAL or simple
  tables, WHERE, ORDER BY, LIMIT), SET (variables with scopes, NAMES,
  CHARACTER SET), USE, BEGIN / START TRANSACTION, COMMIT (with its hint),
  ROLLBACK, with MySQL's operator precedence and bounded nesting.
- Fuzz targets for the packet reader, lexer and parser (run: see below).
  clang 19 with libFuzzer is on node-a, node-b and node-c.
- `kvasir/exec/`: exact decimals and MySQL's text for doubles
  (`decimal.h`); typing and evaluating constant expressions with MySQL
  8.4's result types, NULLs, overflow (1690) and division by zero (NULL,
  1365) (`expression.h`); the system variable registry and a session's
  values with MySQL's defaults, normal forms and refusals, plus `ack_level`
  (`variables.h`); the session: SELECT with no table, SET (evaluating every
  value before assigning any), USE, transactions in the status flags, user
  variables with their types, DATABASE() (`session.h`).
- `kvasir/server/commands.h`: the command phase as a state machine
  (COM_QUERY, COM_PING, COM_INIT_DB, COM_QUIT, COM_RESET_CONNECTION, 1047
  for others), text result sets ended by EOF or by OK under
  CLIENT_DEPRECATE_EOF, one statement per query.
- The diagnostics area (SHOW WARNINGS and SHOW ERRORS with LIMIT and
  COUNT(*), @@warning_count, @@error_count, errors recorded), and the
  session functions connectors call (CONNECTION_ID, VERSION, USER,
  CURRENT_USER, LAST_INSERT_ID, ROW_COUNT, FOUND_ROWS, DATABASE).
- `kvasir/server/connection.h`: a client connection over a `Transport`
  seam (Dannan's stream in the node, a fake in tests), exact-length reads,
  the password checked by a `PasswordChecker` off the strand (the node will
  use Dannan's `WorkerPool`, whose tasks own a Secret).
  `node_settings.h`: the node's settings (no built-in ack_level, TLS
  required, at least one user); `kvasir-node hash-password` makes a
  credential.
- The differential gate, recorded: `tools/record_mysql.sh` runs
  `kvasir/test/differential/session.sql` on the reference server, and
  `differential_test.cpp` replays it through a session: 187 of 187
  statements answer as MySQL 8.4 does. Re-record when the list grows.
- Collations (ruling 32): `tools/kvasir_ducet.cpp` compiles
  `kvasir/data/unicode/allkeys-9.0.0.txt` into the library at build time;
  `kvasir/exec/collation.h` weighs strings by hand under
  utf8mb4_0900_ai_ci, utf8mb4_0900_as_cs, utf8mb4_bin, utf8mb4_0900_bin and
  binary, with sort keys equal to MySQL's WEIGHT_STRING() for 31 observed
  characters. Found on the reference server: MySQL's 0900 collations apply
  none of the table's contractions (Thai and Lao prevowels included). The
  evaluator compares strings under the collation derived for the pair
  (coercibility: literal 4, system variable and session function 3, user
  variable 2; a binary string on either side compares bytes).
  utf8mb3_general_ci and utf8mb4_general_ci (ruling 33):
  `tools/kvasir_general_ci.cpp` derives their weights from
  `UnicodeData-3.0.0.txt`; all 63,488 BMP weights equal the reference
  server's. PAD SPACE pads the shorter string, as MySQL does. LIKE
  matches under the pair's collation, one character at a time. COLLATE
  works, and collations aggregate over every operand as MySQL's do
  (1253, 1267, 1270, 1273).
- `kvasir/catalog/catalog.h`: the node's databases in memory, shared by
  every session (step 2's catalog; in the cluster it becomes agreed state).
  CREATE DATABASE, DROP DATABASE, SHOW DATABASES and USE work as MySQL's.
  Known differences: SHOW DATABASES lists only Kvasir's databases (MySQL
  also lists information_schema, mysql, performance_schema and sys); DROP
  DATABASE IF EXISTS on a missing database keeps its note (MySQL counts one
  and lists none).
- The fuzz targets have run (libFuzzer, clang 19 on node-a, fuzzer and
  UBSan): the lexer's found an empty token, fixed; the packet reader and
  parser ran clean for a minute each. Corpora in `/root/kvasir-work/corpus`
  on node-a.
- Almost every expected type, length, flag and value in `exec_*_test.cpp`
  and the greeting's bytes were observed on the MySQL 8.4.13 container
  (see "Waiting on the maintainer" for where it runs). Known difference: arithmetic on
  a bare NULL literal is typed DOUBLE 23/31 where MySQL reports smaller
  lengths.

**Next, in order**, each with its tests written from the MySQL 8.4 manual
and the reference server first, and watched to fail:

1. The back-off per principal and per source before any hashing: Lugh's Backoff
   moves into Dannan as `dannan::listen::Backoff` (`dannan/listen/backoff.h`,
   on `contract::TimePoint`; agreed by Dannan and Lugh 2026-11-16, stack
   rule 3), in Dannan 0.8.5 (in its gate, 2026-11-16 03:00Z). Kvasir writes
   none of its own; the sign-in path is written against wait(key, now),
   failed(key, now), succeeded(key), with keys "principal:<name>" and
   "source:<address>", a waiting client answered before any hashing. When
   0.8.5 is tagged: move the pin, wire it into the connection.
2. Tables: CREATE TABLE with MySQL's types into the catalog, then INSERT
   and SELECT over in-memory rows (the design's surface order). Then
   SHOW STATUS and the other statements a client's `status` command and
   connectors send.
3. utf8mb4_unicode_ci, and character sets besides
   utf8mb4, utf8mb3 and binary (N'...' and _latin1'...' are refused); an
   alias holding a character past the BMP (MySQL refuses it with 3854,
   Kvasir accepts it).

**A real client works** (2026-11-16): `kvasir-node` on Dannan 0.8.4
(`StreamTransport` over its Stream, `start_tls_unanswered` after exactly the
SSLRequest; `PoolPasswordChecker`, Argon2id on a WorkerPool as foreground
work with a deadline) answered MySQL 8.4.13's own `mysql` client from
another Pi: TLS required and negotiated, statements, databases, 1045 for a
wrong password, 3159 without TLS, a clean stop on SIGTERM. So the OK, ERR
and result-set bytes and the response decoder suit a real client. Still
unchecked: other connectors (JDBC, Python's, Go's), and compression.

**Known**: the stub runs for the session and the command phase crashed
(dereferencing an empty result) instead of reporting each failure; they
failed, but their failure counts were not seen.

**The repository is public** (`github.com/tomdionysus/kvasir`), at the maintainer's
word, 2026-11-15, after a check that no local notes, keys or server
addresses are in its history (his email is there only as the commit
author, as on Lir).

**Hosted CI runs every job on each push** (check, release on x86-64 and
arm64, clang, ASan/UBSan, TSan, coverage, CodeQL). It stopped while the
repository was private (GitHub refused jobs for the account's billing) and
ran again once it was public. Its x86-64 sanitizers are part of the gate:
they found a use-after-free the Pi's arm64 ASan run passed. Dependabot's two
pull requests (actions bumps) are untouched, the maintainer's to merge.

On the Pis: since 2026-11-16 every build Pi runs a 4K-page kernel with a
39-bit address space, where TSan works and ASan does not start (g++ or
clang); until a 48-bit 4K kernel comes, hosted CI's x86-64 ASan is the ASan
gate. clang 19 (thread-safety analysis, libFuzzer) is on node-a, node-b
and node-c.
The maintainer's rules: TSan on the Mac only when hunting a specific bug; no sanitizer
builds on node-d (4 GB).

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends). Sources go by rsync to
  `/root/kvasir-work/kvasir` with Dannan beside them in
  `/root/kvasir-work/dannan`; `chown -R root:root` both after the sync, or
  git refuses the clone. Both exist on node-d (which has clang-format,
  so `tools/ci.sh check` runs there) and node-c (which has not).
  The remote tree is not a git checkout: `git add -A` there before `check`
  and `git reset -q` after.
- `clang-format -i` is run on the Pi and the formatted files copied back.
- Commit when a step's gate passes and push `develop`. No Claude attribution.
- Design questions go to the maintainer through the interactive question tool, with a
  recommendation first. He asked that other sessions' advice be gathered and
  reconciled before he is asked anything.
- A ruling relayed by another session is recorded as relayed, with who
  relayed it, and confirmed with the maintainer when Kvasir's own work turns on it.
- On this machine a hook blocks `cat`, `grep`, `sed`, `awk`, `head`, `tail`
  and `nl` over local files in the shell. Read files with the Read tool,
  search with basemind, and reach a sibling's branch with `git show
  <ref>:<path>` into `build/`.

## With the other sessions

- **Dannan**: agreement is on its `develop` as 0.9, with the simulator's
  first pieces. Kvasir reviewed the simulator design and the reconfiguration
  proposal; every point was taken. Kvasir owes its sequential model and its
  rule per level for which losses which faults excuse, for the simulator's
  checker, at step 3. The slot table with no root except at genesis is ruled
  (relayed by Dannan). Building `transfer()` found a safety gap, fixed with
  the maintainer's approval (relayed by Dannan, 2026-11-15): records are per slot, a
  transfer stays pending until a settle message follows its commit, and a
  change's names must share one slot. Read "Reconfiguration: the gap found
  in building it, and the fix" in Dannan's agreement design before the range
  map (moves, splits) is designed at step 3. Built on Dannan's
  `feature/agreement-reconfiguration` (c524a28), not merged or pushed
  (relayed 2026-11-15). What changed:
  - The caller names the slot, and a change's names share one slot.
  - AcceptorStore keeps per-slot records:
    `load(slot, names, every, done(Held))` and `keep(slot, held, done)`.
  - `transfer()` carries whole values, and refuses a slot holding more
    names than one message carries (`max_names`) until value-less prepare
    and deltas come. Kvasir's ranges, and its rows if they become names,
    must respect that bound.
  - `hint(space, slot)` reads the `agree.slots/<space>` advert. A wrong hint
    costs one refusal, which names the set and its version. Dannan has also taken Kvasir's
  ask to export its fuzz harness with the test fixtures (a 0.9 item, not
  started). Dannan's working tree may be on a feature branch: read
  its designs from `develop`.
- **Mimir**: co-signed the simulator. Shares the level names. Proposed the
  atomic commit across replica sets to Dannan (0.9 item 7,
  `../mimir/TODO/2026-11-15-cross-slot-commit-proposal.md`). Kvasir's review,
  sent to Mimir and Dannan on 2026-11-15:
  - SQL transactions run several rounds, but their writes are known by
    COMMIT.
  - Intents must sit per row inside the range's value, not lock the whole
    register.
  - The conflict rule must be the hosted system's to choose.
  - The record must carry a commit timestamp from the hybrid clock.
- **Dagda**: adopting the level standard and recording it in the stack
  document. Its node-local log (slice 4) is paused for Macha's port; step 5
  waits on it. Dannan asked it which file contract the log writes through,
  so real Dagda can run in the simulator.
- Kvasir edits no sibling's tree.

## Waiting on the maintainer

Nothing blocks step 2. Not yet put to him:

- The default replica count (three proposed).
- The design's "Acknowledgement levels" items marked Proposed.
- The SQL surface left until asked for (the replication protocol, XA,
  full-text and spatial indexes, loadable plugins).
- The MySQL 8.4 server for the differential gate (ruling 30) is up:
  Oracle's `container-registry.oracle.com/mysql/community-server:8.4-aarch64`
  (the plain `8.4` tag is amd64 only), MySQL 8.4.13, container
  `kvasir-mysql84` on node-b, bound to 127.0.0.1:33306 only, started
  2026-11-15 after a note on the claim board and held there as a light
  claim. Its root password is in `/root/kvasir-work/mysql84.env` on node-b
  (mode 600, never copied off). `/root/kvasir-work/probe.sh` there runs SQL
  from stdin as a utf8mb4 client and prints each column's type, collation,
  length, decimals and flags, then the rows. Expected values in tests that
  came from it say "observed on MySQL 8.4.13".

## Ordered work

1. The skeleton. Done.
2. One node, in memory: the protocol, SQL and transactions for the subset
   real drivers and tools use, with the differential gate. 2a in hand.
3. The commit protocol written down and passed through Dannan's simulator.
4. The cluster at `decided` and `replicated`. Waits on Dannan 0.9.0.
5. Persistence in Dagda, with backup and restore. Waits on Dagda's
   node-local log.
6. `ephemeral`, `persisted` and held commits; range splits and merges;
   pieces of a query on other nodes; the Lugh plugin; a change feed.
