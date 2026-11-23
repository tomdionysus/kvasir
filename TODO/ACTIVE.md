# Kvasir - Active Work

Last updated: 2026-11-22 (afternoon), written for a session starting with no memory of
it. What landed, and how, is in `TODO/COMPLETED.md`; this file is the plan.

## Read, in this order

1. This file.
2. `TODO/2026-11-15-kvasir-design.md`: the design, the maintainer's rulings
   1 to 33 and what is still proposed.
3. `TODO/2026-11-20-commit-protocol.md`: the commit protocol, written before
   its code (step 3).
4. `TODO/2026-11-15-ack-levels-standard.md`: the four acknowledgement level
   names, a standard for the whole stack (this file is its text).
5. `TODO/COMPLETED.md`: what landed, with gate counts, newest first.
6. Before work that touches the cluster or another project: Mimir's design
   (`../mimir/TODO/2026-11-15-mimir-design.md`), the stack document
   (`../dagda/TODO/2026-11-03-stack.md`), and Dannan's `docs/config.md`.
7. `TODO/2026-11-15-design-review.md` only for the sessions' measurements
   and warnings on the first draft. It is a record, not the design.

## Where it stands (2026-11-22)

- **Release 0.1.0** (tag on `main`, 2026-11-20): one node, in memory, MySQL
  compatible as the README lists, `ephemeral` only. Kvasir is a prototype
  until it has a durable store and has been tested on a cluster (the
  maintainer, 2026-11-20): no package is published, and the README says so.
- **`develop`** is ahead of `main`: Dannan 0.10.0 and its adoptions, the
  node's logging and setting names, the commit protocol for one range
  (step 3), Connector/J's metadata against a node (step 1d), the fuzz
  targets on Dannan's harness, a real MySQL client library driving a node
  in the tests, the SQL writes (INSERT IGNORE, REPLACE, ON DUPLICATE KEY
  UPDATE, INSERT ... SELECT, locking reads, shared and range locks),
  SERIALIZABLE, folded row versions, and joins (inner, outer, USING,
  NATURAL, grouped, locking). Last gate (node-c, 2026-11-23): release,
  ASan and TSan each **425 tests, 1224 of 1224** differential statements;
  hosted CI green, coverage 99.2% of lines.

## Rulings in force

Each the maintainer's, on the date given (relayed rulings say who relayed).

- **Persistence is Dagda** (2026-11-20): each node keeps its catalog, rows
  and accounts in Dagda's store, memory a cache; nothing durable any other
  way.
- **No single-node code** (2026-11-20): a leaderless cluster that may be
  one node; nothing is built that a second node would replace.
- **ACID at every node**, levels by name (the ack-levels standard). A store
  may offer a subset, refusing the rest by name; 0.1.0 offers `ephemeral`
  only.
- **Releases** (2026-11-20): when current work is done, rationalise the
  docs (README on installing and using), build the debs, run the full
  tests with sanitizers and fuzzing, tag on `main`, merge, push, keep the
  release badge. Minor or patch bumps only. Versions always in full x.y.z.
  No packages published while Kvasir is a prototype.
- **Branches** (2026-11-20, relayed): each feature on `feature/<name>`,
  gated there, squash-merged to `develop` and pushed.
- **Sanitizers** (2026-11-20, relayed): ASan and TSan once before merging a
  feature with code changes, on a 15 GB build node (never node-d, 4 GB);
  also to hunt a bug. Every stack library a sanitizer build links is built
  from source instrumented (Dannan's `DANNAN_SANITIZE`).
- **Tests**: GoogleTest; from the MySQL 8.4 manual and the reference server,
  watched to fail first; a test is never deleted. Test once; no flaky tests.
- **Coverage ~100%** (2026-11-20).
- **Three replicas by default** (2026-11-22): a range's rows on three nodes,
  surviving any one lost with a majority left.
- **Next while the store and cluster wait** (2026-11-22): the SQL writes,
  INSERT IGNORE, REPLACE, ON DUPLICATE KEY UPDATE and INSERT ... SELECT.
- **Joins next** (2026-11-23): after the SQL writes, SERIALIZABLE and
  version folding, joins come before subqueries, UNION and temporal types.
- **Temporal types next** (2026-11-23): after joins, DATE, TIME, DATETIME,
  TIMESTAMP and YEAR come before subqueries and UNION.
- **Joins run a fixed plan** (2026-11-23: "Fixed plan, record gaps"): tables
  in written order, outer joins as written, nothing reordered; the
  temporary-table flags follow from that plan. Rows, values, types,
  nullability and errors match MySQL exactly; where MySQL's cost-based
  optimizer picks another order (row order with no ORDER BY, key flags
  dropped by a temporary table), the difference is recorded, and the
  gate's join statements carry ORDER BY.
- **Hosted CI** (2026-11-20): keep it green; check after each push; it runs
  only on `develop`, `main` and pull requests, never for TODO, docs or
  Markdown alone, a newer push cancelling the older run, CodeQL on `main`
  and weekly.
- **Settings** (2026-11-20): YAML is the source; flags and variables
  override, named from the key (`a.b` is `--a-b` and `KVASIR_A_B`, Dannan's
  `docs/config.md`); `log.level` and `log.format` are the stack's.
- **A real MySQL client library in the tests only** (2026-11-20, 2026-11-21):
  Connector/C++ 26.10.0 and libmysqlclient 8.4.13, built from Oracle's
  source everywhere.
- **Cluster time**: book only for a job ready to run; release the moment it
  ends; work others wait on comes first.
- **Nothing identifying in tracked files** (2026-11-20): "the maintainer",
  node-a to node-d, "the coordinating session"; real names only in
  `CLAUDE.local.md`.

## Next, in order

1. **The real MySQL client in the tests: what is left.** Landed on
   `feature/mysql-client` (2026-11-22, see COMPLETED): libmysqlclient
   8.4.13 and Connector/C++ 26.10.0 built from source by
   `cmake/mysql_client.cmake`, and `kvasir::server::Node` driven by them
   (sign-in by both paths, refusal, text and prepared statements, metadata,
   rollback, errors); STRCMP added for getTables, checked on the reference
   server and in the gate; LIMIT and OFFSET placeholders read as MySQL reads
   each bound type. The transport's coverage: an empty command packet, a
   statement past 16 MiB (which found generated column names uncut), a
   peer that hangs up. Left in connection.cpp: the lock-wait timer and the
   wake that comes before the wait (both want Dannan's injected time in the
   connection), and a payload error the reader can no longer raise.
1a. **The SQL writes** (the maintainer's choice, 2026-11-22, while 2 to 4
   wait): INSERT IGNORE, REPLACE, ON DUPLICATE KEY UPDATE, locking reads and
   INSERT ... SELECT all landed 2026-11-22 (see COMPLETED); left: a
   prepared INSERT ... SELECT (its parameters typed at the prepare). The
   design they were built to, kept for the record of why (the maintainer,
   2026-11-22: "Shared locks first"):
   - **The store's locks gain a shared mode.** A lock name is held either
     by one transaction exclusively or by any number in shared mode; a
     transaction alone in shared mode may take it exclusively (upgrade).
     A writer meeting shared holders waits for one of them (the store
     names it), and asks again when woken: a cycle through another holder
     is found on that next wait. Locks are released at commit, rollback, or
     with an undone statement's, as now.
   - **SELECT ... FOR UPDATE / FOR SHARE / LOCK IN SHARE MODE**, with OF
     tables, NOWAIT (3572) and SKIP LOCKED: a locking read reads the current
     rows (the latest committed and the transaction's own, as UPDATE does),
     not the snapshot, and locks each row that passes WHERE, exclusive or
     shared; a row another transaction holds makes the statement wait
     (1205 at innodb_lock_wait_timeout, 1213 on a cycle) as a write does.
     Only the rows that pass WHERE are locked, as InnoDB locks when a key
     finds them (the maintainer, 2026-11-22: "Rows that pass WHERE"); InnoDB
     without an index locks every row examined, a recorded difference.
   - **Range locks first** (the maintainer, 2026-11-22: "Build range locks
     first"): a locking read also locks the primary-key range it read, so
     another transaction's insert into that range waits, as InnoDB's
     next-key locks make it. The range comes from WHERE where it bounds the
     primary key (=, IN, <, <=, >, >=, BETWEEN in a conjunction); anything
     else locks the table's whole key range, as InnoDB's scan without an
     index does. A range lock is shared among readers and blocks only
     inserts (and keys moved into it) by others. SERIALIZABLE (served
     2026-11-23) takes these locks for every plain SELECT in a transaction
     (BEGIN, or autocommit off), as InnoDB's FOR SHARE; a SELECT that is its
     own transaction stays a consistent read.
     Locking reads observed on MySQL 8.4.13 (two PyMySQL sessions,
     `build/connectors/py_locks*`): a plain read never waits; FOR UPDATE and
     FOR SHARE NOWAIT on a held row are 3572; SKIP LOCKED leaves held rows
     out; a wait ends at innodb_lock_wait_timeout with 1205; shared locks
     share and block an UPDATE; a locking read sees the latest commit where
     the snapshot does not; FOR UPDATE OF an unknown table is 3568, a table
     in two clauses 3569; LOCK IN SHARE MODE NOWAIT and NOWAIT SKIP LOCKED
     are 1064.
   - **INSERT ... SELECT** reads its source as FOR SHARE under REPEATABLE
     READ and SERIALIZABLE, as a consistent read under READ COMMITTED (the
     manual, "Locks Set by Different SQL Statements in InnoDB"), and inserts
     what it read with IGNORE, REPLACE or ON DUPLICATE KEY UPDATE as a
     VALUES list would; the source may be the target table (MySQL reads it
     whole first).
   Each observed on the
   reference server first and put in
   the gate, each ACID as every statement is (a REPLACE's delete and insert,
   an upsert's read and write, one statement's whole effect or none).
1b. **Joins** (the maintainer's choice, 2026-11-23), each stage on its own
   branch, gated and merged. J1 to J5 landed 2026-11-23 (see COMPLETED);
   left: multi-table UPDATE and DELETE.
   - J1, the parser: JOIN, INNER JOIN, CROSS JOIN, STRAIGHT_JOIN, LEFT and
     RIGHT [OUTER] JOIN, NATURAL [LEFT | RIGHT] JOIN, ON, USING, comma
     lists and parentheses, as a tree (a comma binds looser than JOIN, so
     `a, b JOIN c ON a.x = c.x` is 1054 in the on clause). The executor
     still refuses (1235).
   - J2, inner joins: nested loops in written order over each table's rows;
     names resolve across the tables in scope (1052 ambiguous, 1054 with
     the clause named, 1066 a repeated table or alias, 1051 for an unknown
     `t.*`); `*` and `t.*` expand in written order; WHERE, ORDER BY,
     DISTINCT and LIMIT over the joined rows; each column's metadata from
     its own table.
   - J3, outer joins and USING / NATURAL: NULL-extended rows; the outer
     side's columns lose NOT_NULL; USING and NATURAL's columns once, first,
     coalesced (RIGHT's from the right table), 1054 'from clause' for an
     unknown USING column.
   - J4, grouping over joins (COUNT over the NULL-extended side counts 0).
   - J5, locking reads and SERIALIZABLE over joins: each table's rows locked
     by its own clause (OF names a table or alias).
   - Later: multi-table UPDATE and DELETE.
   Observed on MySQL 8.4.13 (2026-11-23): a temporary table drops key
   flags, and a join goes through one when sorted other than by the plan's
   first table with each later table found through an index, when grouped
   other than off such an index, or under DISTINCT; with no ORDER BY the
   order is the plan's (see the known differences).
1c. **Temporal types** (the maintainer's choice, 2026-11-23), each stage on
   its own branch, gated and merged, every behaviour observed on the
   reference server first:
   - T1a, CAST and CONVERT, which Kvasir lacks altogether: to SIGNED,
     UNSIGNED, CHAR[(n)] [CHARACTER SET], BINARY[(n)], DECIMAL[(m[,d])],
     DOUBLE, FLOAT[(p)], and CONVERT(x USING charset).
   - T1b, temporal values in expressions: DATE, TIME, TIMESTAMP literals,
     CAST and CONVERT to DATE, TIME, DATETIME (with fsp), comparison and
     ordering, the result metadata (DATE, TIME, DATETIME types, lengths,
     decimals); NOW, CURDATE, CURTIME, CURRENT_TIMESTAMP, SYSDATE, UTC_*
     from an injected clock (the statement's time, fixed for its length).
   - T2, columns: DATE, TIME[(fsp)], DATETIME[(fsp)], TIMESTAMP[(fsp)],
     YEAR in CREATE TABLE; stored text and key order; INSERT and UPDATE
     conversion from strings and numbers (1292 in strict mode, warnings
     else; NO_ZERO_DATE, NO_ZERO_IN_DATE); DEFAULT CURRENT_TIMESTAMP and ON
     UPDATE CURRENT_TIMESTAMP; information_schema's columns.
   - T3, TIMESTAMP's time zone: stored in UTC, shown in the session's
     time_zone (SYSTEM and offsets; named zones refused until zone data
     exists).
   - T4, the date and time functions. Done: T4a, the part functions (YEAR,
     MONTH, DAY and the rest, DATEDIFF, LAST_DAY, DAYNAME); T4b, INTERVAL
     arithmetic (DATE_ADD, DATE_SUB, ADDDATE, SUBDATE, TIMESTAMPADD, +
     INTERVAL), TIMESTAMPDIFF and EXTRACT. Left, T4c: DATE_FORMAT,
     STR_TO_DATE, UNIX_TIMESTAMP, FROM_UNIXTIME, WEEK() and its kin
     (YEARWEEK, WEEKOFYEAR, with modes), ADDTIME, SUBTIME, TIMEDIFF,
     MAKEDATE, MAKETIME, SEC_TO_TIME, TIME_TO_SEC.
   - T5, the binary protocol: temporal parameters of COM_STMT_EXECUTE and
     temporal columns of binary result rows (what Connector/J and
     Connector/C++ prepared statements send and read).
2. **Step 2b: each node's state in Dagda**, in the cluster's shape. Waits on
   Dagda's durable AcceptorStore, which is not in its 0.4.0 (tagged
   2026-11-23, 94a1b60, carrying the register map). Dagda's
   answer (2026-11-20), the shape to build:
   - pin Dagda exactly, fetched as Dannan is; use `dagda::Context` from a
     `dagda::Node` over Kvasir's Dannan node, never LocalStore;
   - rows per range, each range one register in Dannan's agreement, its
     acceptors on Dagda's durable AcceptorStore; a commit is one agreed
     change (step 3's code, `kvasir/txn/range_commit.h` and
     `range_proposer.h`, wired under the session at COMMIT);
   - folding writes immutable, content-addressed objects (snapshots and
     deltas, at most 16 MiB each) at `Durability::persisted`, and each
     range's manifest under "manifest/<range>" in the every-node head
     space, by the register map's conditional write; never a manifest
     commit per transaction;
   - a range's replica set is every node (one, here).
3. **Commits across ranges**, on Dannan's atomic commit across replica sets
   (its plan item 26, for 0.12.0). Dannan writes the design first; Kvasir
   and Mimir confirm it before code. Until then such a transaction is
   refused (1235); Kvasir writes no cross-range protocol of its own.
4. **The cluster** at `decided` and `replicated`; ranges on Dannan's
   replica sets, moves by `transfer()`; the range map in slot registers.
5. **Backup and restore** in Dagda to a point in time.
6. `persisted` and held commits; range splits and merges; pieces of a
   query on other nodes; the Lugh plugin; a change feed.

Waiting on nothing, to take when the above is blocked:
- Dannan's pin to 0.11.1 (the pin target, relayed 2026-11-23; Dagda 0.4.0
  is built on it), at the next release gate.
- SQL: subqueries, unions;
  utf8mb4_unicode_ci and other character sets; temporal types.
- Differences recorded, to probe and build: a character past U+FFFF in a
  name (MySQL turns it into `?`, warnings 1300 and 3854); an intermediate
  DECIMAL past 65 digits (MySQL keeps it, Kvasir refuses 1690).
- Coverage's last lines (session.cpp, expression.cpp, connection.cpp's
  lock-wait timer on Dannan's injected time).
- Dannan 0.10.0's adoptions not taken, and why: `Completion<T>` and
  `Outcome<T>` (no callback type of Kvasir's own yet; they come with the
  cluster's calls); MemoryStore, RetryPolicy::unbounded, RandomId, sha1,
  the durable directory helpers, base64 and base32 (nothing in Kvasir does
  their work); the fuzz build keeps its own global sanitizer flags.
- The Status/Admin dashboard: designed by Kvasir now, built once Dannan's
  HTTP core (its item 24) and Lugh's admin-ui package land. Asked for: an
  optional, configurable HTTP API with OpenAPI and a React front end,
  served SPA-style from a configurable directory, Lugh's admin styling,
  sign-in by password or by Lugh.

## Known differences from MySQL 8.4

Each to be closed, or kept by a decision.

- Temporal values (2026-11-23, to close in T1b's rest and T2): NOW(6) and
  the fsp clock functions show 000 in their last three digits until
  Dannan 0.12.0's microsecond wall clock; time_zone SYSTEM is UTC (the
  reference server's system_time_zone is UTC too); named zones
  ('Europe/London') are 1298, where the reference server has zone data
  loaded and takes them; a TIME cast to DATE or
  DATETIME (today's date) is refused; COALESCE, IF, CASE and GREATEST over
  mixed temporal types are not yet typed as MySQL types them (DATETIME in
  utf8mb4_0900_ai_ci, length 76).

- Joins (kept by the fixed-plan ruling, 2026-11-23): Kvasir reads the
  first table written first (a RIGHT JOIN's right side); a sorted join
  keeps key flags when the sort keys are that table's and each later
  table is found through an index (an equality on a key's leading column
  with what the tables before it hold), else it goes through a temporary
  table. That matches every case observed on MySQL 8.4.13 but where its
  optimizer reads another table first: `a JOIN b ON a.id = b.a_id ORDER BY
  b.y` keeps b's key flags in MySQL (b read first, a by its key), Kvasir
  drops them. A one-row table, which MySQL reads as a constant, may differ
  too. With no ORDER BY the row order is the plan's. The gate writes the
  table sorted by first.

- Not yet checked on the reference server: FLOAT operands of the flow
  control functions (Kvasir merges them as DOUBLE); LPAD or RPAD with a
  NULL length (Kvasir: a LONG BLOB); STRCMP's result column (Kvasir: a
  signed BIGINT of length 2, as MySQL's source reads).
- A handshake asking for a character set no session speaks (latin1,
  utf8mb3) leaves the server's utf8mb4; MySQL takes it. Character set and
  collation variables refuse what Kvasir does not convert (1235) and
  collation ids (1232); MySQL takes both.
- REPEAT of a utf8mb3 system variable past 16 MB is a LONG_BLOB; MySQL's is
  a MEDIUM_BLOB of 16777215.
- A constant string read as an integer ('2x') warns once; MySQL twice.
- A FLOAT inside CONCAT: Kvasir writes it as a FLOAT column shows (six
  digits, fixed from 1e-15 to below 1e15); MySQL's CONCAT turns to exponent
  notation sooner (1e14 as '1e14', 1.5e-10 as '1.5e-10').
- Arithmetic on a bare NULL literal is DOUBLE 23/31; MySQL's lengths are
  smaller.
- A statement that waited for a lock lets go of its earlier locks and takes
  them again; MySQL's keeps them.
- Locking reads lock the rows WHERE keeps (InnoDB without an index locks
  every row it examines) and, for a key range, only a single-column integer
  primary key's bounds in WHERE narrow it; any other WHERE locks the whole
  table's keys from inserts, where InnoDB would lock only an index's range.
  A prepared INSERT ... SELECT is refused (1235).
- GROUP_CONCAT, the JSON aggregates and WITH ROLLUP are refused (1235);
  1056 names the function in upper case.
- An engine other than InnoDB is refused as unknown. Column names compare
  case-insensitively in ASCII only. utf8mb3_bin columns cannot yet be
  compared in expressions. Rows without ORDER BY come in key or insertion
  order, where MySQL may read an index.
- COM_STATISTICS: slow queries, opens and table counts are 0.
- SHOW DATABASES lists Kvasir's databases and information_schema, not
  mysql, performance_schema or sys.
- An alias holding a character past the BMP is accepted (MySQL: 3854).
- information_schema: only SCHEMATA, TABLES and COLUMNS; TABLES' times are
  refused until temporal types; its storage figures are Kvasir's own.
  TABLES lists none of information_schema's own views (MySQL lists each as
  a SYSTEM VIEW; found 2026-11-22).
- Cursors (COM_STMT_FETCH) are refused.

## How work is done

- No builds or tests on the Mac. Every build runs on a build node under a
  claim-board claim (`claims_list`, `claim_take` with the cores the `-j`
  needs, `claim_release` when it ends), `nice 19`. node-c holds a current
  tree in `/root/kvasir-work/kvasir` with build-ci/{release,clang,asan,
  tsan}; node-d (4 GB) has clang-format and a tree; node-a a coverage tree.
  Which node is which, and their addresses, are in `CLAUDE.local.md`.
- No preferred node (the maintainer's rule, relayed 2026-11-23): a job
  takes whichever node has the cores free now, never waiting for one
  because it holds a warm tree. Send the tree there and build from scratch;
  a missing tool or -dev package is installed there (1 core, `nice -n 19
  apt-get install -y <pkg>`, then a board note naming it and the node).
  Wait only when no node has the cores, naming them all (`claim-board wait
  --any`). The one exception is a job that must run on one node (a service
  living there), said so in the claim's job text; never ASan on node-d.
- Send sources with `rsync -a --exclude .git --exclude 'build/' --exclude
  'build-ci/' --exclude 'build-deb/' --exclude CLAUDE.local.md --exclude
  basemind.toml`, then touch the changed files on the node (the clocks
  differ). `tools/ci.sh <step>` with `KVASIR_CI_JOBS=<cores>`; steps:
  check, release, clang, tidy, asan, tsan, coverage, fuzz, deb.
- A red run: the new tests with HEAD's library sources (`git show
  HEAD:<path>` into the node's tree), then the fix.
- Format on node-d after the rsync (`clang-format -i <files>` there), then
  copy the files back; never pipe code through ssh.
- The differential gate is recorded on the Mac, where the reference server
  runs: `sh tools/record_mysql.sh kvasir/test/differential/session.sql >
  build/session.expected.new`, then copy it over `session.expected`.
  Probes: `cd build/mysql84 && sh probe.sh` with SQL on stdin.
- The tests' MySQL client libraries (`cmake/mysql_client.cmake`): built
  once per build node into `build-ci/mysql-client` (about 11 minutes at
  -j1 on a Pi, download included); a node needs pkg-config and
  libtirpc-dev (on node-b and node-c since 2026-11-22). Their sources sit
  in `build-ci/*/mysql-client-build/` for reading what a client sends.
- Real drivers: `build/connectors/` (Connector/J in Docker on the Mac,
  `e2e-jdbc/Meta.java`; Go and Python programs beside it), against the
  reference server on the `kvasir-probe` network and against a throwaway
  `kvasir-node` on a build node (run under `timeout`).
- Simulation: Dannan's simulator and checkers (`dannan/test/fixtures/sim/`,
  `dannan::testing`) run inside `kvasir_tests`.
- A shell hook blocks grep, sed, awk, head, tail and cat on code (they are
  fine on logs, Markdown, text and command output). Read code with the
  Read tool and search it with basemind; never route round the hook with
  an interpreter.
- Commit when a step's gate passes; no Claude attribution; push `develop`;
  check hosted CI.
- Design questions go to the maintainer through the question tool, with a
  recommendation, after other sessions' advice is gathered. A ruling
  relayed by another session is recorded as relayed.

## With the other sessions

- **Dannan**: 0.11.0 exports the node settings reader (ruled 2026-11-22):
  every key under cluster.* (cluster.key, .bootstrap, .listen.*,
  .identity.*, .custody.* and the timing keys), added to Kvasir's reader by
  dannan::config::add_node_settings(reader, "KVASIR") and read by
  node_config(settings), in dannan/config/node_settings.h. Kvasir's own
  listen.* (its MySQL listener) does not collide; Dannan's note names
  mysql.* as the form for a project's own listener, to consider at the
  cluster step. kvasir-node has no copy of the reader. Released: Dannan
  0.11.0 and 0.11.1 are tagged (relayed by the coordinating session,
  2026-11-23); 0.11.0 also adds Fault::cancelled after failed (Kvasir
  switches over no Fault yet).
- **Dannan**: Kvasir is on 0.10.0; its pin target is 0.11.1. Its atomic
  commit across replica sets (item 26) is planned for the minor after
  0.12.0, design first (relayed 2026-11-23). Its HTTP core (item 24) and the
  credential and node helpers come in later minors. Kvasir's commitments
  to its agreement: one name per change; changes repeatable by commit id;
  a range's value kept under `max_value` (256 MiB) by splitting; a crashed
  acceptor that answers from memory restarts not ready and catches up
  before voting.
- **Dannan**: readiness and health (its item 7, 0.12.0; drawn at its
  TODO/2026-11-23-readiness-health.md, f8c9242), confirmed by Kvasir
  2026-11-23: kvasir-node refuses new clients while health() is not_ready
  (incomplete startup included), not while degraded; on draining it stops
  taking connections, moves proposer stickiness away, lets open
  transactions end within a deadline, then closes sessions; after startup
  it pushes a "kvasir.ranges" check (not_ready while a range has not caught
  up, degraded while a replica set is short).
- **Manannan** (the maintainer's ruling, relayed 2026-11-23): the cluster's
  DNS is Manannan's, Dannan's DNS responder (stack item 25) dropped, and a
  founding seed stays HOST:PORT. Kvasir's needs sent to Manannan the same
  day: one service name for MySQL clients (A/AAAA and _mysql._tcp SRV) over
  every node that serves them, following health (in while ready or
  degraded, out while draining or not_ready), short TTLs, and per-node
  names its certificates can carry. Asked back: how a hosted system
  registers, and the name scheme. Wanted from Kvasir's distribution stage.
- **Dannan**: the node-program helper (its item 14, 0.12.0; drawn at its
  TODO/2026-11-23-node-program.md, 61c292d), confirmed by Kvasir
  2026-11-23. kvasir-node moves onto it when Kvasir joins the cluster, not
  at 0.12.0. Asked: named subcommands (hash-password), a default port for
  add_listener_settings (3306, keys mysql.listen.* and mysql.tls.*,
  renaming today's listen.* and tls.*), and what a drain step past its
  deadline does (assumed: on_stop runs, and rolls back what is open).
- **Dagda**: 0.4.0 (register map) is tagged (2026-11-23, 94a1b60), built on
  Dannan 0.11.1. The durable AcceptorStore step 2b waits on is not in it;
  the coordinating session has asked Dagda to put it first (relayed
  2026-11-23). Dagda's objects offer no `decided`.
- **Mimir**: shares the level names; co-proposed the cross-range commit
  that Dannan now owns.
- **Lugh**: @lugh/admin-ui (Lugh's develop 05ebbaf, out in its next tag) is
  the dashboard's shell: TypeScript and CSS in lugh/ui, a file: dependency
  at a Lugh tag compiled by Kvasir's Vite, reset.css then admin.css at the
  root, React 19, react-dom and react-router 7 as peers; AdminShell, NavBar,
  Messages, Confirm, ApiClient, useApi/useLoad/useAct, SignIn (password,
  Lugh with PKCE, or both), DataTable with useCursorPages.
- **Dannan**: microseconds on dannan::contract::WallClock are queued for its
  0.12.0 (answered 2026-11-23; a new virtual changes the vtable, so not a
  patch): `unix_us()` beside `unix_ms()`, defaulting to unix_ms() * 1000;
  SystemWallClock and the stepped test wall clock in microseconds. Until
  then Kvasir uses unix_ms() for NOW(6) and the fsp defaults, the last three
  digits 000 a recorded difference; Dannan says when 0.12.0 has it.
- **Lugh**: 0.6.0 (2026-11-23, tagged on main) is on Dannan 0.11.0 (Boost.JSON's
  compiled part is Dannan's, none in lugh_client), handles Fault::cancelled
  (a switch over Fault needs it), and is the tag @lugh/admin-ui is depended
  on by file path at; its CHANGELOG has the rest. Nothing of Kvasir links it
  yet: the Lugh plugin and the dashboard take 0.6.0 when they start.
- **Lugh**: 0.5.0 (2026-11-22) is the lugh_client the Lugh plugin starts
  on: its Fault, Outcome and Completion are dannan::contract's, with
  unavailable, timed_out and invalid (an unreadable reply); lugh::undecided
  covers failed, unavailable and timed_out. lugh-node's flags take the
  stack's names since 0.4.0.
- **The coordinating session** orders cluster time and relays the
  maintainer's words to every session. Kvasir edits no sibling's tree.

## Waiting on the maintainer

Nothing blocks Kvasir's next item. Open, none blocking:
- Two figures Kvasir gave Dannan as proposals: a range splits at about
  64 MiB of folded data; a range is unavailable to writes for at most
  about 1 s while it moves.
- The design's "Acknowledgement levels" items marked Proposed.
- The SQL surface left until asked for (the replication protocol, XA,
  full-text and spatial indexes, loadable plugins).
