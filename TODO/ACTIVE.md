# Kvasir - Active Work

Last updated: 2026-11-19, written for a session starting with no memory of
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

Step 2a, one node speaking the MySQL protocol, is in hand. Last gate
(2026-11-20, node-d): release **367 passed, 0 failed**, the
differential gate **694 of 694**. Server-side prepared statements work
with Go's, Python's and Java's drivers, answering as MySQL does. No claim is held and no wait is queued.
The evaluator's types are each expression's own; what a client is told is
`reported()`'s, in character_set_results (COMPLETED, 2026-11-19).

**Before any work: the build cluster was wiped on 2026-11-18** (the maintainer, relayed
by the coordinating session). Checked on 2026-11-19, all four Pis:

- They have g++ 14, clang 19, cmake, ninja, Boost, OpenSSL, yaml-cpp,
  GoogleTest, lcov, git and rsync, on the 6.18.62-v8-va48+ kernel.
- They have no `/root/kvasir-work`, no Docker and no clang-format. The
  MySQL 8.4.13 reference server (`kvasir-mysql84`) is gone, with its
  password.
- Saved off node-b before the wipe, in `build/pi-rescue/` (not in git, so do
  not clean `build/`): `probe.sh`, the probe SQL files, the collation
  checks (`verify/`, with the reference server's general_ci dump) and
  `node-b-listing.txt` (the container's image and port).

Since then (2026-11-19, at the maintainer's word): the reference server runs in Docker
on the Mac (`CLAUDE.local.md` has how to probe and record), and re-recording
the 605 statements on it gave byte-identical answers. clang-format is on
node-d. Setting up a tree on another Pi:

1. (Done on node-d.)
2. Put a tree on one 15 GB Pi under a claim: rsync this repository to
   `/root/kvasir-work/kvasir`, `git clone
   https://github.com/tomdionysus/dannan.git /root/kvasir-work/dannan`
   (the pin is tag 0.9.0), `chown -R root:root` both, then
   `KVASIR_CI_JOBS=2 KVASIR_DANNAN_REPOSITORY=/root/kvasir-work/dannan nice
   -n 19 tools/ci.sh release`. It must give 337 passed before anything
   changes (a first run on a new machine, the stated reason for running
   the suite again).
3. Format on node-d, or install clang-format on another Pi (the maintainer: "You can
   install clang-format on any cluster pi"; a note on the board first).

**Done and pushed**:

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
  (COM_QUERY, COM_PING, COM_INIT_DB, COM_QUIT, COM_RESET_CONNECTION,
  COM_STATISTICS from the node's `NodeStatistics`, 1047 for others), text result sets ended by EOF or by OK under
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
  `differential_test.cpp` replays it through a session: 605 of 605
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
- The fuzz targets have run (libFuzzer, clang 19 on node-a, fuzzer and
  UBSan): the lexer's found an empty token, fixed; the packet reader and
  parser ran clean for a minute each. Their corpora went with the wipe.
- Aggregates, GROUP BY, HAVING and DISTINCT; the flow control functions
  (IFNULL, COALESCE, NULLIF, IF, CASE); the string functions (CONCAT,
  CONCAT_WS, LENGTH, CHAR_LENGTH, SUBSTRING, LEFT, RIGHT, TRIM, LTRIM,
  RTRIM, REPLACE, LOCATE, INSTR, POSITION, LPAD, RPAD, REPEAT, REVERSE) and
  the number functions (ABS, SIGN, FLOOR, CEILING, ROUND, TRUNCATE, MOD),
  the last two groups in `kvasir/exec/functions.cpp`. Any other function
  is refused.
- Almost every expected type, length, flag and value in `exec_*_test.cpp`
  and the greeting's bytes were observed on the MySQL 8.4.13 reference
  server.

**A step's gate** is the release build's full suite on a Pi plus the
differential gate, nothing more. No sanitizer build (ASan, UBSan, TSan) is
part of any step's gate: sanitizers run only before a release at the maintainer's
word, or to hunt a named bug that cannot be pinned any other way
(CLAUDE.md, Tests). Earlier entries in COMPLETED that list ASan counts were
outside that rule (the maintainer, 2026-11-17).

**The stack's converged plan** (2026-11-20, agreed by Kvasir; the record
is "The converged plan" -> "Kvasir" in
`../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`):
1. Done: Dannan pinned at 0.9.2; Dannan's SteppedTime in the tests.
2. When Dannan's next minor carries them: credentials onto Dannan's
   helper (the PHC string, `argon2id$v=19$m=,t=,p=$salt$hash`, unpadded
   base64, replacing Kvasir's hex form; make, verify, needs_rehash,
   constant work with a decoy), and the ack-level enum from Dannan's
   contract/.
3. Become a hosted system in a Dannan node through Dannan's node helper
   (its item 14), with status and readiness.
4. Fuzz targets and a fake Stream from Dannan's testing exports.
5. Ranges on Dannan's transfer-driven replica sets (step 4); Dagda's
   durable AcceptorStore at step 5; clients reach the cluster by DNS that
   every node answers (the maintainer's question 3).
6. Stays Kvasir's: the lock manager, the little-endian codec, and the
   MySQL, SQL, decimal and collation code.
Dannan's keyed map and Backoff get a hard bound (least recently touched
evicted); Kvasir already keys by name and address together.
Items 2 to 4 wait on Dannan's next minors, which have not started: the maintainer told
Dannan to stop until he has answered every open question (relayed by Lir
coordinating session, 2026-11-20). Kvasir's own SQL work is not held.

**Next, in order**, each with its tests written from the MySQL 8.4 manual
and the reference server first, and watched to fail:

1. JDBC's metadata calls (DatabaseMetaData.getTables, getColumns): the
   queries are in `build/connectors/prep.log`. Probed 2026-11-20:
   information_schema.TABLES has 21 columns and COLUMNS 22, with MySQL's
   own metadata for each (enum-backed CHAR columns flagged ENUM, BLOB-typed
   DATA_TYPE and COLUMN_TYPE, TIMESTAMP and DATETIME columns CREATE_TIME,
   UPDATE_TIME, CHECK_TIME, storage figures such as DATA_LENGTH 16384). In
   order, each its own step:
   a. (Done 2026-11-20.) UPPER and LOWER, every code point as MySQL's.
   b. (Done 2026-11-20: already answered as MySQL's; 13 statements added
      to the gate.) HAVING without GROUP BY over the select list's aliases.
   c. information_schema as a read-only database. The shape (decided
      2026-11-20, Kvasir's own choices where it differs, each a known
      difference):
      - SCHEMATA, TABLES and COLUMNS are built from the catalog when a
        statement names them: a view is a catalog::Table definition, the
        type MySQL reports for each column (explicit, as MySQL's are not
        derived: TABLE_TYPE a CHAR 44 flagged ENUM, DATA_TYPE a BLOB
        201326580, SQL_PATH a NULL column), and its rows as text; the
        select code runs over it as over a table, with no transaction.
      - Any other of MySQL 8.4's 79 views (ADMINISTRABLE_ROLE_AUTHORIZATIONS
        ... VIEW_TABLE_USAGE; the list is in COMPLETED when this lands) is
        refused with 1235 naming it; a name MySQL does not have is 1109
        "Unknown table 'NOPE' in information_schema" (42S02).
      - USE information_schema works and DATABASE() names it; creating,
        changing or dropping anything in it is 1044 "Access denied for user
        '...'@'...' to database 'information_schema'"; SHOW DATABASES lists
        it.
      - TABLES' CREATE_TIME (TIMESTAMP), UPDATE_TIME and CHECK_TIME
        (DATETIME) are refused with 1235 when selected (SELECT * included)
        until Kvasir has temporal types; JDBC's queries do not select them.
      - Storage figures: TABLE_ROWS Kvasir's committed count and
        AUTO_INCREMENT its next value, where MySQL's are InnoDB's estimates;
        ENGINE InnoDB, VERSION 10, ROW_FORMAT Dynamic as MySQL's; the byte
        sizes 0. The gate leaves these columns out.
      MySQL's metadata for all three views was probed (a table of nine
      column types: INT AUTO_INCREMENT key, VARCHAR NOT NULL DEFAULT,
      DECIMAL, TEXT, TINYINT(1), BIGINT UNSIGNED ZEROFILL, DOUBLE, CHAR
      _bin, VARBINARY); probe COLUMNS' values for each column type and
      key kind before writing the tests.
   d. JDBC's two queries in the differential gate, then Connector/J's
      getTables and getColumns against kvasir-node.
   Also: the fuzz target for the execute reader has not run yet (a 15 GB
   Pi with clang); cursors (COM_STMT_FETCH) are refused.
2. (Done 2026-11-20; kept for its notes.) Server-side prepared statements. Logged 2026-11-20
   (`build/connectors/prep.log`): Go's driver prepares on the server for
   every query with arguments (COM_STMT_PREPARE, COM_STMT_EXECUTE,
   COM_STMT_CLOSE), so every Go program passing arguments fails on Kvasir
   today; mysql-connector-python with `prepared=True` too (and
   COM_STMT_RESET); Connector/J only with `useServerPrepStmts=true` (by
   default it sends text). The bytes, captured through a proxy
   (`build/connectors/capture/`: `proxy.py`, `capture.txt`, and
   `NOTES.md`, which decodes every packet: prepare OK, parameter and
   column definitions, the execute packet, binary rows). In pieces, each
   its own gated commit:
   a. The protocol (`kvasir/protocol/`): the prepare reply, the execute
      packet read as untrusted input (bounded by the statement's
      parameter count, types kept from the last bind when the
      new-params-bound flag is 0; temporal types refused, Kvasir has
      none), binary rows from a result's text values by column type
      (TINY 1 byte, SHORT 2, LONG and INT24 4, LONGLONG 8, FLOAT 4, DOUBLE
      8, everything else length-encoded; a NULL bitmap offset by 2). A
      fuzz target for the execute reader. Tests from the manual and the
      captured bytes.
   b. The session: placeholders numbered in the parser (a COM_QUERY with
      `?` is MySQL's 1064: record it in the gate), prepare (an id per
      session from 1, at most max_prepared_stmt_count, 1461 past it; the
      statement checked, as MySQL refuses an unknown table at prepare),
      execute with bound values, close, reset. A bound parameter's type
      (observed): an integer LONGLONG 21 BINARY (no NUM flag), a string
      VAR_STRING in the connection's collation 65532 long, NULL the type
      inferred at prepare. At prepare (observed): beside an integer
      column or in arithmetic with an integer, LONGLONG 21; beside a
      string, alone in the select list, or anything else, VAR_STRING
      65532; an INSERT's VALUES take the column's kind. Prepare-time
      result columns are typed without running the statement. LIMIT ?
      and cursors (execute flags other than 0; COM_STMT_FETCH) refused
      at first, with known differences recorded.
   c. The command phase: COM_STMT_PREPARE, EXECUTE, SEND_LONG_DATA,
      CLOSE (no reply), RESET; 1243 for an unknown statement id.
   d. Real drivers against kvasir-node on a Pi: Go's (the probe in
      `build/connectors/go-cap/`), mysql-connector-python prepared, and
      Connector/J with useServerPrepStmts, each answering as the
      reference server did.
   Before (a): capture more bound types (unsigned, FLOAT, DOUBLE alone,
   a DECIMAL string, a long string, TINY) and SELECT ? with each, so the
   rules above rest on observation.
   JDBC's metadata calls (getTables, getColumns) query
   INFORMATION_SCHEMA.TABLES and COLUMNS (in prep.log): a later item.
2. UPPER and LOWER, which need Unicode's case mapping.
3. Folding old row versions away (memory grows with every change today;
   folding what is older than every open snapshot is the design's, and
   must come before long runs).
4. SERIALIZABLE (range read marks), INSERT IGNORE, REPLACE, ON DUPLICATE
   KEY UPDATE, INSERT ... SELECT.
5. utf8mb4_unicode_ci, and character sets besides utf8mb4, utf8mb3 and
   binary (N'...' and _latin1'...' are refused).

**Known differences from MySQL 8.4**, each to be closed or kept by a
decision:

- Not yet checked on the reference server: FLOAT operands of the flow
  control functions (Kvasir merges them as DOUBLE); the type of LPAD or
  RPAD with a NULL length (Kvasir: a LONG BLOB, as REPEAT with a NULL
  count).
- A handshake asking for a character set no session speaks (latin1,
  utf8mb3) leaves the server's utf8mb4; MySQL takes it.
- REPEAT of a utf8mb3 system variable past 16 MB is a LONG_BLOB; MySQL
  reports a MEDIUM_BLOB of 16777215.
- A constant string argument read as an integer ('2x' as a position)
  warns once; MySQL warns twice.
- Arithmetic on a bare NULL literal is typed DOUBLE 23/31; MySQL reports
  smaller lengths.
- A statement that waited for a lock lets go of the locks it took before
  the wait and takes them again; MySQL's keeps them.
- GROUP_CONCAT, the JSON aggregates and WITH ROLLUP are refused (1235).
  1056 names the function in upper case. DISTINCT with ORDER BY on a
  column not selected is not refused (MySQL: 3065). A grouped result's
  metadata follows MySQL's temporary-table rules as observed; where MySQL
  would pick an index, which Kvasir has none of, it may differ.
- An engine other than InnoDB is refused as unknown (MySQL makes MyISAM
  and MEMORY tables). Column names compare case-insensitively in ASCII
  only. utf8mb3_bin columns cannot yet be compared in expressions. Rows
  without ORDER BY come in primary-key (or insertion) order, where MySQL
  may read a covering index.
- Character set and collation variables refuse what Kvasir does not
  convert (latin1, utf8mb3: 1235) and numbers (collation ids: 1232); MySQL
  takes both.
- COM_STATISTICS: Slow queries, Opens, Flush tables and Open tables are 0
  (no slow log, no table cache); Threads leaves out MySQL's event
  scheduler.
- SHOW DATABASES lists only Kvasir's databases (MySQL also lists
  information_schema, mysql, performance_schema and sys). DROP DATABASE IF
  EXISTS on
  a missing database keeps its note (MySQL counts one and lists none).
- An alias holding a character past the BMP is accepted (MySQL: 3854).

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

**Hosted CI** runs on each push: check, release on x86-64 and arm64, clang,
coverage and CodeQL. The ASan/UBSan and TSan jobs were removed at the maintainer's
word (6eb0322, 2026-11-17); `tools/ci.sh asan` and `tsan` remain for a
release or a bug hunt. Worth knowing when one is run: x86-64 once found a
use-after-free that arm64's ASan passed (argument evaluation order
differs), so a release's sanitizer run wants both architectures.
Dependabot's two pull requests (actions bumps) are untouched, the maintainer's to
merge.

The maintainer's sanitizer rules: only before a release or to hunt a named bug; TSan
on the Mac only when hunting a specific bug; none on node-d (4 GB).

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends). Sources go by rsync to
  `/root/kvasir-work/kvasir` with Dannan beside them in
  `/root/kvasir-work/dannan`; `chown -R root:root` both after the sync, or
  git refuses the clone. Neither exists on any Pi since the wipe (see
  "Resume here"). Touch the changed files on the Pi after an rsync: the
  clocks differ, and ninja otherwise misses them. An incremental run is
  `nice -n 19 ninja -C build-ci/release -j2 kvasir_tests`, then the
  binary. `tools/ci.sh` reads `KVASIR_CI_JOBS`, not `JOBS`.
- To watch a test fail when the code is already written: send the tests
  with HEAD's versions of the sources (`git show HEAD:<path>` into
  `build/head/`), build, see the failures, then send the new sources.
- Formatting is clang-format on a Pi that has it (node-d), the file
  piped through over ssh and copied back.
- Record the differential gate on the Mac, where the reference server
  runs: `sh tools/record_mysql.sh kvasir/test/differential/session.sql`,
  its output becoming `session.expected`; statements recorded before must
  come back unchanged.
- Commit when a step's gate passes and push `develop`. No Claude attribution.
- GitHub CI (the maintainer, 2026-11-20, relayed by the coordinating session): "If you
  have public github repos, fix and maintain github CI." Kvasir's is
  public: check `gh run list -R tomdionysus/kvasir` after each push and fix
  any failure before other work.
- Branches (the maintainer, 2026-11-20, relayed by the coordinating session): "features
  and milestones should be developed on branches and merged to develop on
  completion", and "Prefer squash commits for the entire feature or
  milestone, onto develop". Each step is a `feature/<name>` branch, its
  gate passed there, squashed onto develop as one commit and pushed.
- Testing (the maintainer, 2026-11-20, relayed by the coordinating session): "All
  builds and tests must run on the cluster. ASan/TSan etc are to be run
  only on the completion of a feature or milestone before merge to
  develop; it is also acceptable to run them to diagnose bugs where other
  methods have failed. Do not run multiple mutants in testing in general,
  unless the code change is small and/or high risk. Do not run dozens of
  test runs, test once and be done with it. Do not tolerate flaky tests.
  Investigate tests that randomly fail." So a feature with code changes
  runs `tools/ci.sh asan` and `tsan` on a 15 GB Pi (never node-d) on its
  branch before the squash. This replaces "sanitizers only before a
  release". UPPER and LOWER (450b26f) merged before this arrived without
  a sanitizer run.
- Where functionality lives across the stack (the maintainer, 2026-11-20, relayed by
  the coordinating session): the projects negotiate through the coordinating
  session, which keeps the record in
  `lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md`. A
  question is first tested against the goals (no duplication; the deepest
  shared dependency owns what two or more need; no special role or node;
  failures never the user's problem) and the group's resources; only what
  they cannot settle goes to the maintainer. Each project gets its list when the plan
  converges. Kvasir touches nothing under discussion until then.
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
    `local`, else forwards the batch to a replica. A delta is taken only
    at exactly its base (and summary), a replay at the new ballot answered
    as kept (confirmed). Reads, in part 6 too: a `change_deltas` with no
    delta commits without an accept when every promiser names the same
    accepted ballot and summary, so a current read is one round trip with
    no accept; otherwise it falls back to a ballot-only accept. The read's
    promises are kept like any prepare's (Dannan, for 0.9.0), so a current
    read costs one round trip and one sync per promiser. Kvasir's
    suggestion of unkept promises was unsafe as put: the fallback accept,
    built at the read's ballot, could overwrite a lower-ballot write
    committed by an acceptor that lost the promise in a crash. Dannan
    records the safe form for after 0.9.0 (an unkept read-only prepare
    that returns only on matching answers, else starts again with a kept
    one), with a simulator run of those crashes.
    Added at Mimir's review, accepted by Kvasir: `Deltas::decline` (Kvasir
    declines when not `local` and forwards); `Deltas::summaries`, the
    summary keep() will set, which the fast path checks with the ballot
    (Kvasir computes it from base summary and delta, and must test the two
    computations agree; a mismatch costs a round, not safety); DeltaChange
    runs on the agreement's strand and never waits on a store (Kvasir's
    row checks come before the call); a need_whole acceptor is caught up by
    an identity change(), or by Kvasir's own copy first.
  - Dannan's review against the CASPaxos paper (2026-11-17, branch
    feature/attempt-timeout, Kvasir reviewed): a retry could apply a
    change twice, and a change of several names could be half taken.
    Fixes: each accepted state carries its ancestry (at most 16 ballots,
    which Kvasir's range store persists with its state); a change whose
    retry cannot tell ends `Outcome::unknown` unless repeatable. Kvasir's
    batches are repeatable by their own marks: every commit in a range
    keeps its transaction id, and the DeltaChange (run only when `local`)
    returns no delta when its batch is already in the base. Multi-name
    changes go through plan item 7's atomic commit (a transaction record),
    which is Kvasir's design for commits across ranges; Kvasir will use
    Dannan's item 7 and never names more than one range per change.
    Merged on Dannan's develop (ea84a31, unpushed until the maintainer says): a
    repeatable change is always run on the retry's own base; change(),
    change_deltas() and forget() take one name until item 7.
  - The simulation's transport seam (Dannan, 2026-11-17, reviewed by
    Kvasir): whole nodes in the world through a call-level
    `net::Transport`, the node's loops on an injected schedule; sessions,
    TLS and the scheduler outside. Kvasir's simulations drive sessions
    directly (statements as values), the MySQL listener staying outside.
    Kvasir's timers (lock-wait timeout, the proposer's batching window,
    catch-up retries) take `Context::time().after(delay, fn)`, which piece
    2a routes through the world's schedule when one is set (Dannan,
    answered). Kvasir's connection lock timer is Asio's today; it moves to
    `Context::time().after()` when the node runs inside a Dannan node.
    Built (Dannan develop d50bb67): `dannan/test/fixtures/sim/nodes.h`,
    `SimNodes`, real nodes on the world's transport, time and schedule,
    with restarts keeping or wiping state, partitions and heals, and
    per-node clock skew; tests in `dannan/test/sim_nodes_test.cpp`. This
    is where step 3's simulation of Kvasir's commit protocol will run.
  - Dannan 0.9.0 is being cut (2026-11-17, the maintainer). Kvasir told Dannan the
    tag needs only what is on Dannan's develop (agreement with
    change_deltas, the simulation harness, Context::time().after() on the
    schedule), and that the open items can wait for 0.10.0 (atomic commit
    across replica sets, the peer-witnessed high-water mark, the exported
    fuzz harness, loader promotion) or are not used (DTLS/UDP). After the
    tag: move the pin from 0.8.5 to 0.9.0 and report what breaks.
    Confirmed on Dannan's develop (df4b2d8; the tag waits on the maintainer): all of
    it there; Backoff, StreamListener, start_tls_unanswered and ClassGates
    unchanged; WorkerPoolConfig gains `schedule`, empty by default. Using
    SimNodes means linking `dannan::sim_checkers` and defining
    DANNAN_TEST_TEMP_ROOT. Changes from 0.8.x: Dannan's CHANGELOG.md.
    Tagged 2026-11-17 (main 59b7abc); Kvasir's pin moved to 0.9.0 the same
    day. One break: a test's WorkerPoolConfig aggregate lacked the new
    `schedule` member (-Werror=missing-field-initializers).
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
- **Dagda**: the maintainer has ruled (relayed by Louhi and Dagda, 2026-11-20) that
  Dagda builds a durable AcceptorStore for Dannan's agreement, one for
  every hosted system, in its release after 0.3.2. Kvasir told Dagda how
  its use differs: deltas applied to an exact base with the ballot as one
  durable step, ancestry of 16 ballots, values up to 256 MiB (whole
  records rare), group commit across slots including the promises a
  current read keeps, order from the agreement only, and later a log
  trimmable after folding. Asked about a per-space "caught up" signal:
  Kvasir needs it for the manifest head space only, as a query and a
  notice, to defer claim releases and folding's drops while not caught
  up; correctness does not rest on it.
- **Dagda**: adopting the level standard and recording it in the stack
  document. Its node-local log (slice 4) is paused for Macha's port; step 5
  waits on it. Dannan asked it which file contract the log writes through,
  so real Dagda can run in the simulator.
- Kvasir edits no sibling's tree.

## Waiting on the maintainer

Nothing blocks. The MySQL 8.4 reference server (ruling 30) is MySQL 8.4.13
in Docker on the Mac (the maintainer, 2026-11-19), Oracle's
`container-registry.oracle.com/mysql/community-server:8.4.13`, container
`kvasir-mysql84`, 127.0.0.1:33306 only; `CLAUDE.local.md` has the rest.
Expected values in tests that came from it say "observed on MySQL 8.4.13";
a newer 8.4 patch release must re-record the differential statements and
explain any that change.

For him, none blocking:

- Two audits the maintainer asked for (relayed by the coordinating session,
  2026-11-20): no duplicated functionality across the stack, and no
  special role or node. Kvasir's reports went to the coordinating session,
  which correlates them; nothing moves until the maintainer decides. Kvasir's main
  points: tests define SteppedTime where Dannan has one; kvasir-node runs
  its own process lifecycle and console output instead of being a hosted
  system in a Dannan node (supervise, status, logging); the credential
  format may duplicate Lugh's (it would belong in Dannan); and a client's
  address is the one place a lost node still reaches the user, which
  every client-facing system shares, so Kvasir proposed a cluster entry
  point in Dannan.

- The default replica count (three proposed).
- The design's "Acknowledgement levels" items marked Proposed.
- The SQL surface left until asked for (the replication protocol, XA,
  full-text and spatial indexes, loadable plugins).

## Ordered work

1. The skeleton. Done.
2. One node, in memory: the protocol, SQL and transactions for the subset
   real drivers and tools use, with the differential gate. 2a in hand.
3. The commit protocol written down and passed through Dannan's simulator.
4. The cluster at `decided` and `replicated`, on Dannan 0.9.0 (pinned).
5. Persistence in Dagda, with backup and restore. Waits on Dagda's
   node-local log.
6. `ephemeral`, `persisted` and held commits; range splits and merges;
   pieces of a query on other nodes; the Lugh plugin; a change feed.
