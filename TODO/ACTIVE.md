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

**Done and pushed** (318 tests; sanitizers run on the Pis and in hosted
CI's x86-64 jobs):

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
  `differential_test.cpp` replays it through a session: 433 of 433
  statements answer as MySQL 8.4 does. Re-record when the list grows.
- Tables and rows on one node: CREATE / DROP TABLE, SHOW TABLES, INSERT,
  UPDATE, DELETE, SELECT over a table's rows, and transactions with lock
  waits in `kvasir/txn/store.h` (see COMPLETED, 2026-11-16).
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

1. Lock waits are done (see COMPLETED): a waiting statement is undone and
   run again once woken. Known difference: MySQL's statement keeps the
   locks it took before the wait; Kvasir's lets them go and takes them
   again.
2. UPDATE and DELETE are done (see COMPLETED). Next: the functions
   clients use most (COUNT and the other aggregates, GROUP BY, string and
   number functions), then SHOW STATUS and the other statements a
   client's `status` command and connectors send. Old versions are never
   folded away yet (memory grows with every change; folding older than
   every open snapshot is the design's, to come before long runs).
3. SERIALIZABLE (range read marks), INSERT IGNORE, REPLACE, ON DUPLICATE
   KEY UPDATE, INSERT ... SELECT.
   Known differences now: an engine other than InnoDB is refused as
   unknown (MySQL makes MyISAM and MEMORY tables); column names compare
   case-insensitively in ASCII only; utf8mb3_bin columns cannot yet be
   compared in expressions; rows without ORDER BY come in primary-key (or
   insertion) order, where MySQL may read a covering index instead.
4. utf8mb4_unicode_ci, and character sets besides
   utf8mb4, utf8mb3 and binary (N'...' and _latin1'...' are refused); an
   alias holding a character past the BMP (MySQL refuses it with 3854,
   Kvasir accepts it).

**A real client works** (2026-11-16): `kvasir-node` on Dannan 0.8.4
(`StreamTransport` over its Stream, `start_tls_unanswered` after exactly the
SSLRequest; `PoolPasswordChecker`, Argon2id on a WorkerPool as foreground
work with a deadline) answered MySQL 8.4.13's own `mysql` client from
another Pi: TLS required and negotiated, statements, databases, 1045 for a
wrong password, 3159 without TLS, a clean stop on SIGTERM. On Dannan
0.8.5 the sign-in asks `dannan::listen::Backoff` (Lugh's, moved into Dannan
by stack rule 3) per principal and per source before any hashing. So the OK, ERR
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

On the Pis: since 2026-11-16 (~10:30Z) all four run a 4K-page, 48-bit-VA
kernel, where ASan, UBSan and TSan all work (g++ everywhere, clang 19 on
node-a, node-b and node-c). Hosted CI's x86-64 sanitizers remain part of
the gate: they found a use-after-free arm64 missed.
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
    costs one refusal, which names the set and its version.
  - Value-less prepare and deltas (Dannan's design, "Value-less prepare
    and deltas", reviewed by Kvasir and Mimir 2026-11-16, Dannan 857f9dd,
    built next): prepare answers carry a ballot, a has-value bit and a
    64-byte store summary (Kvasir: the range's version, rows, bytes); the
    base is fetched by ballot, in pieces resumable by offset; an accept may
    carry a delta; a name an acceptor holds at the base ballot is neither
    fetched nor sent, so catching a range's new acceptors up first makes
    transfer a ballot check. A delta is one commit's row changes. Changed
    in building part 4 (Dannan ca7ad31, reviewed by Kvasir 2026-11-16 and
    taken): no separate `apply()`, which could leave a value at a ballot
    its recorded state did not say after a crash; deltas go into `keep()`
    (a held state may carry a delta and its base), which applies them and
    records the ballots as one durable step, all or nothing, answering
    `Kept{kept, need_whole}` (need_whole non-empty means nothing kept); a
    repeated accept already kept is answered by the rule from the recorded
    state, the store not asked; `keep()` is asynchronous, never inline.
    Final interface reviewed (Dannan 1c624d3, 2026-11-16): the store side
    fits ranges; ballot-only accepts make a caught-up transfer value-less.
    Kvasir does not need the whole resend in pieces for 0.9.0: it keeps a
    range's value under `max_value` (256 MiB) itself, splitting before it,
    and later holding only the manifest head and unfolded tail, and catches
    lagging acceptors up before they are sent anything. The value-less
    change Kvasir asked for is in 0.9.0 at the maintainer's word (part 6, reviewed by
    Kvasir 2026-11-16): `change_deltas(..., DeltaChange)`, the change given
    each name's base (ballot, summary, has_value, and `local`: this node's
    acceptor holds exactly it) and returning deltas only; commit on a
    majority that took it; need_whole nodes named in `Outcome::need_whole`
    for Kvasir to catch up, never resent; the fast path keeping ballots,
    not bytes, and checking the base by ballot only (right for ranges,
    which change only through agreement). Kvasir makes a delta only when
    `local`, else forwards the batch to a replica. Asked (open, may follow
    0.9.0): a read with no deltas that commits without an accept when a
    majority promised one base, so a current read is a round trip with no
    sync.
  Dannan has also taken Kvasir's
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
