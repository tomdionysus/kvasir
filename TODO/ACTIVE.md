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
  in the tests, STRCMP, and fixes. Last gate (node-c, 2026-11-22): release,
  clang, ASan and TSan each **401 tests, 966 of 966** differential
  statements; hosted CI green on 8a53e58, coverage 99.3% of lines.
- **Nothing is running and no claim is held.** Stopped 2026-11-22 15:10Z:
  what is next needs the reference server, and Docker on the Mac was not
  running (starting it is the maintainer's to do).

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
   wait): INSERT IGNORE and REPLACE (landed 2026-11-22), then INSERT ... ON
   DUPLICATE KEY UPDATE and INSERT ... SELECT (and REPLACE ... SELECT with
   it), each observed on the
   reference server first and put in
   the gate, each ACID as every statement is (a REPLACE's delete and insert,
   an upsert's read and write, one statement's whole effect or none).
2. **Step 2b: each node's state in Dagda**, in the cluster's shape. Waits on
   Dagda's durable AcceptorStore (after its 0.4.0, which carries the
   register map; 0.4.0 waits on the maintainer's word to tag). Dagda's
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
- Dannan's pin to its latest 0.10.x patch (0.10.2, tag f36d697; none of
  0.10.1's or 0.10.2's changes reaches Kvasir), at the next release gate.
- Folding old row versions away in the node's store (memory grows with
  every change).
- SQL: SERIALIZABLE (range read marks), INSERT IGNORE, REPLACE, ON
  DUPLICATE KEY UPDATE, INSERT ... SELECT; joins, subqueries, unions;
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
- A FLOAT column shows the shortest text that reads back as the same float
  (1.2345678, 3.4028235e38); MySQL shows six significant digits (1.23457,
  3.40282e38, 16777200) and computes with the float widened to a double
  (f + 0 is 1.2345677614212036). Found 2026-11-22 by INSERT IGNORE's
  FLOAT limit.
- Arithmetic on a bare NULL literal is DOUBLE 23/31; MySQL's lengths are
  smaller.
- A statement that waited for a lock lets go of its earlier locks and takes
  them again; MySQL's keeps them.
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

- **Dannan**: (2026-11-22, asked by Lir) Dannan may export the reader of
  its node settings (dannan::NodeConfig: cluster.*, node.*, listen.*,
  custody.*, identity.*) in 0.11.0; kvasir-node has no copy and will take
  it when it joins a cluster. Kvasir's own listen.host and listen.port name
  its MySQL listener: if Dannan's keys include listen.* for the node's
  transport, one of the two must move (Kvasir's under mysql.* proposed).
- **Dannan**: Kvasir is on 0.10.0. Its atomic commit across replica sets
  (item 26) is for 0.12.0, design first. Its HTTP core (item 24) and the
  credential and node helpers come in later minors. Kvasir's commitments
  to its agreement: one name per change; changes repeatable by commit id;
  a range's value kept under `max_value` (256 MiB) by splitting; a crashed
  acceptor that answers from memory restarts not ready and catches up
  before voting.
- **Dagda**: 0.4.0 (register map) awaits its tag; the durable AcceptorStore
  follows in a 0.4.x or 0.5.0 on Dannan 0.10.x, and Dagda will send its
  design and date. Dagda's objects offer no `decided`.
- **Mimir**: shares the level names; co-proposed the cross-range commit
  that Dannan now owns.
- **Lugh**: 0.5.0 (2026-11-22) is the lugh_client the Lugh plugin starts
  on: its Fault, Outcome and Completion are dannan::contract's, with
  unavailable, timed_out and invalid (an unreadable reply); lugh::undecided
  covers failed, unavailable and timed_out. lugh-node's flags take the
  stack's names since 0.4.0.
- **The coordinating session** orders cluster time and relays the
  maintainer's words to every session. Kvasir edits no sibling's tree.

## Waiting on the maintainer

Nothing blocks Kvasir's next item. Open, none blocking:
- Dagda 0.4.0's tag (Dagda's to put to him).
- Two figures Kvasir gave Dannan as proposals: a range splits at about
  64 MiB of folded data; a range is unavailable to writes for at most
  about 1 s while it moves.
- The design's "Acknowledgement levels" items marked Proposed.
- The SQL surface left until asked for (the replication protocol, XA,
  full-text and spatial indexes, loadable plugins).
