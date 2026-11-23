# Kvasir - Completed

The record, newest first. Each entry says what landed, with its gate counts.

## 2026-11-22 (evening): STRCMP checked, LIMIT placeholders as MySQL reads them

- With the reference server back (its container had stopped with Docker):
  STRCMP's result column is a LONGLONG of two, as built; its statements
  and Connector/C++'s getTables expression are in the gate.
- A bound LIMIT or OFFSET, observed through mysql-connector-python's
  binary protocol (`build/connectors/py_limit*.py`, answers beside them):
  a negative integer 1690 (22003, "... in 'mysqld_stmt_execute'"), a
  DOUBLE or DECIMAL 1210 ("Incorrect arguments to mysqld_stmt_execute"),
  NULL 0, a string as strtoull reads it, no warnings. The node now does the
  same; the session and Connector/C++ tests watched failing first.
  (Connector/J reports 1690's SQLSTATE as 22001, its own relabelling.)
- Found: information_schema.TABLES lists none of its own views (a gate
  statement first written against it; listed under Known differences).
- TSan's step now loads Dannan's suppressions from the fetched source: a
  run halted on Asio's executor count (a release decrement and an acquire
  fence, which g++'s TSan does not model), the blind spot Dannan already
  names. Kvasir keeps no suppressions of its own.
- Gate (node-c): release, ASan and TSan each **403 tests, 975 of 975**
  statements, TSan with no reports.
- The transport's coverage (a coverage run first: 99.3%, 61 lines left,
  17 in the transport): an empty command packet (1047, as MySQL through
  PyMySQL), a statement past 16 MiB through Connector/C++, a peer that
  hangs up. The large statement found that the node named its column with
  the whole 17 MiB expression and the client dropped the connection;
  generated names are now cut to 255 bytes as MySQL cuts them. Recorded:
  MySQL also cuts an alias past 256 bytes, with warning 1466.
- Gate (node-c): release, ASan and TSan each **407 tests, 976 of 976**
  statements, TSan with no reports.
- Names as MySQL keeps them (observed, then MySQL's Name_string::copy read
  as a guide): leading blanks and control characters go from every name;
  an alias keeps 256 bytes, a generated name 255, a character whole; an
  alias changed warns 1466 (its text as written) or 1474 when nothing is
  left, ahead of the SELECT's own warnings.
- Gate (node-c): release, ASan and TSan each **408 tests, 980 of 980**
  statements, TSan with no reports.
- DISTINCT with an ORDER BY naming a column the list does not hold: 3065,
  as MySQL (the known difference closed).
- Gate (node-c): release, ASan and TSan each **409 tests, 993 of 993**
  statements, TSan with no reports.
- The maintainer's rulings (2026-11-22, through the question tool): three
  replicas by default; the SQL writes next while the store and the cluster
  wait.
- INSERT IGNORE, the first of the SQL writes: duplicates skipped (1062,
  the AUTO_INCREMENT value handed on), NOT NULL columns given their
  implicit default (1048, 1364), values stored as MySQL stores them without
  strict mode with the warnings strict mode would raise (store.h's lenient
  value), the OK's affected rows and info as PyMySQL saw them. Found: FLOAT
  display (six significant digits in MySQL), recorded.
- Gate (node-c): release, ASan and TSan each **410 tests, 1033 of 1033**
  statements, TSan with no reports.
- REPLACE: each row a new row would duplicate, by any unique key, deleted
  (a current read) and the row inserted; affected rows, Duplicates and the
  insert id as PyMySQL saw them; REPLACE IGNORE and REPLACE ... ON
  DUPLICATE KEY UPDATE are syntax errors as in MySQL. With it, the OK's
  insert id carries an explicit AUTO_INCREMENT value when none was
  generated, for INSERT too.
- Gate (node-c): release, ASan and TSan each **411 tests, 1052 of 1052**
  statements, TSan with no reports.
- INSERT ... ON DUPLICATE KEY UPDATE: the row a key holds updated through
  an upsert column source (the held row by the table's names, the new row
  by VALUES(column), warning 1287, or the row alias and its column names);
  1, 2 or 0 affected, Duplicates, the OK's id and the AUTO_INCREMENT hand-on
  as PyMySQL saw them; 1062 for an update that collides, IGNORE's warnings.
- Gate (node-c): release, ASan and TSan each **412 tests, 1076 of 1076**
  statements, TSan with no reports. CI green on 9661d49; coverage 99.2% of
  lines (8,692 of 8,766), functions 98.8%.
- The maintainer's ruling (2026-11-22): shared locks and locking reads
  before INSERT ... SELECT; the design is in the plan (item 1a).
- Locking reads and INSERT ... SELECT (the maintainer's rulings: shared
  locks first, rows that pass WHERE, range locks first):
  - the store: shared and exclusive row locks (an upgrade for a holder
    alone), range locks that keep others' inserts and moved keys out, a
    range refused over another's uncommitted write; undone with a statement;
  - SELECT ... FOR UPDATE / FOR SHARE / LOCK IN SHARE MODE with OF, NOWAIT
    (3572) and SKIP LOCKED, reading the current rows, locking the rows WHERE
    keeps and the key range WHERE bounds on a single-column integer primary
    key (the whole table's otherwise; none under READ COMMITTED), 3568 and
    3569 as MySQL; grouped reads too;
  - INSERT ... SELECT and REPLACE ... SELECT: the source read whole first,
    inside the statement, as FOR SHARE (a consistent read under READ
    COMMITTED); AUTO_INCREMENT reserved 1, 2, 4 at a time; the info line
    always; the OK's id only a generated value; a copied column too long
    1265, an expression 1406 (all observed through two PyMySQL sessions and
    the gate).
- Gate (node-c): release, ASan and TSan each **418 tests, 1115 of 1115**
  statements, TSan with no reports.
- Coverage after the SQL writes fell to 98.9% (CI on 2bf918f); tests for
  what they left (an upsert's DEFAULT and IGNOREd collision, a leading
  number's exponent, a number too long to read, an upsert waiting on a held
  row, a failed source, the key range's other forms) bring it to **99.2%**
  of lines (8,981 of 9,055); the node already answered each as MySQL does.
  Release: 419 tests, 1128 of 1128 statements.
- FLOAT shown as MySQL shows it (a known difference closed): six
  significant digits for a column and a FLOAT result, a FLOAT(M,D)
  result's D decimals, the value kept and computed at full precision.
  Found with it and recorded: GROUP BY a primary key reports expression
  columns NUM (MySQL BINARY NUM); CONCAT of a FLOAT.
- Gate (node-c): release, ASan and TSan each **419 tests, 1139 of 1139**
  statements, TSan with no reports. CI green on 561c44d, coverage 99.2%.
- Grouped columns typed as MySQL types them when it groups off an index
  (the leading columns of any index): the list's expressions keep their own
  types; a temporary table otherwise, and always under SELECT DISTINCT.
- Gate (node-c): release, ASan and TSan each **419 tests, 1151 of 1151**
  statements, TSan with no reports.
- Old row versions folded away (the plan's "waiting on nothing" item): live
  snapshots registered with the store; a row keeps its newest version and
  each live snapshot's; a deleted row goes once no snapshot sees it; rows a
  snapshot held fold when it ends. The gate caught a version moved onto
  itself (emptied) before it merged.
- Gate (node-c): release, ASan and TSan each **420 tests, 1151 of 1151**
  statements, TSan with no reports. CI green on 74fa6c5.
- SERIALIZABLE served: REPEATABLE READ with every plain SELECT in a
  transaction (BEGIN, or autocommit off) read FOR SHARE, its rows and key
  range locked; a SELECT that is its own transaction stays a consistent
  read. The 1235 refusal is gone. Red first: the new gate statements were
  refused (1161 of 1165).
- Gate (node-c): release, ASan and TSan each **420 tests, 1165 of 1165**
  statements, TSan with no reports.

## 2026-11-22: A real MySQL client library drives a node in the tests (step 1, most of it)

- Measured first on a build node, one claim per step: libmysqlclient
  8.4.13 client-only builds in 334 s at -j1 (230 edges; needs pkg-config
  and libtirpc-dev, no bison); Connector/C++ 26.10.0 WITH_JDBC in 824 s
  with its X DevAPI, far less without it. Found: 26.10.0's merged static
  library (`libmysqlcppconn-static.a`) holds only libmysqlclient's objects,
  none of the driver's; the driver's own `libjdbc.a` links.
- `cmake/mysql_client.cmake`: both fetched by URL and SHA-256, the client
  and the classic API only, built once into `KVASIR_MYSQL_CLIENT_ROOT` (658 s
  at -j1 on a Pi, download included) and found there by every later
  configure; `KVASIR_MYSQL_CLIENT_TESTS` (off in the Debian build); hosted
  CI caches the libraries per pin and architecture; the tidy step builds
  them before reading the compile commands.
- `kvasir::server::Node`: kvasir-node's assembly moved into the library,
  so a test serves a node on a loopback port.
- `server_mysql_client_test.cpp`, 7 tests through Connector/C++: sign-in
  over TLS by the full path and then the fast path, a wrong password (1045,
  28000), text statements and their column metadata, prepared statements
  in the binary protocol, DatabaseMetaData's getTables and getColumns, a
  rollback, an unknown table (1146, 42S02).
- Found: getTables compares with STRCMP, which the node refused. STRCMP
  added from the manual's examples, watched failing first. Its result
  column is not yet checked against the reference server, which was not
  running (Docker on the Mac was down), nor is the negative `LIMIT ?`
  through COM_STMT_EXECUTE.
- Found while writing the tests, not a difference: Connector/C++ writes any
  binary DOUBLE as "%f" would (3.500000) in getString, whatever the server
  sent.
- libtirpc-dev installed on node-b and node-c (board notes posted).
- Gate (node-c, 2026-11-22): release, clang, ASan and TSan each **401
  tests, 966 of 966** statements, TSan with no reports; tidy clean.
- Hosted CI red twice after the push, fixed the same hour: its trixie image
  lacked ncurses' headers, which MySQL's configure checks for even for the
  client (6335ab5); and under CXX=clang++ MySQL tested its flags with
  clang++ and gave them to gcc, so the libraries are now built with the
  system's cc and c++ whatever the tree uses (8a53e58, proved from scratch
  under clang++ on node-b first). CI green on 8a53e58; coverage **99.3% of
  lines** (8,350 of 8,412), functions 99.0%.
- Branches: the merged `feature/coverage` deleted locally and on GitHub
  (its work was on develop under the history scrub's hashes).

## 2026-11-20 to 22: Dannan 0.10.0, the node's settings, and the stack's housekeeping

- Dannan 0.10.0 (fa35ad8), read in full before the repin: its ack-level
  names, `encoding::utf8_length` in place of three UTF-8 scanners of
  Kvasir's own, `encoding::hex`, `DANNAN_SANITIZE` for the sanitizer steps
  (which check both libraries carry the runtime's symbols), Dannan's
  `install-debs` in the deb step, its tools not built. The fuzz targets
  moved onto Dannan's harness (d4c61e6).
- kvasir-node (07c593a): its lines through Dannan's `SwappableSink` and a
  scoped logger; `log.level` and `log.format` (the stack's names, agreed
  with Dannan's session); every flag and variable named from its key
  (`--listen-host`, `--listen-port`, `--users`; `users` has no variable,
  since a credential holds commas).
- A collation of another character set is refused before a character set
  not yet held (ea246e1), found when a gate statement was pushed untested
  in a version-fix commit (`git commit -a` swept it in) and turned develop
  red; fixed the same day.
- Hosted CI runs less (081e3da): not for TODO, docs or Markdown alone; a
  newer push cancels the older run; CodeQL on main and weekly.
- The ack-levels standard: a store may offer a subset of the levels,
  refusing the rest by name (c92aba1, agreed with Dagda).
- The README says Kvasir is a prototype and drops the apt install (no
  package is published until a durable store and a tested cluster);
  CODE_OF_CONDUCT.md added; GitHub's description and topics set at the
  maintainer's word.
- History rewritten twice at the maintainer's word to remove identifying
  data (names, hosts, the coordinating session's name, once split across a
  line); main, develop and the 0.1.0 tag force-pushed; backups in
  `~/devroot/backups/2026-11-20-kvasir.tar.gz`.
- The real MySQL client library: Connector/C++ 26.10.0 and libmysqlclient
  8.4.13 from Oracle's source, ruled; stopped before the first build.
- The plan rewritten (2026-11-22) as the current plan only; its history is
  here and in the design.

## 2026-11-21: Step 1d: Connector/J's getTables and getColumns against a node

- Connector/J 9.1.0's DatabaseMetaData.getTables and getColumns (four
  calls) against kvasir-node on a build node and against MySQL 8.4.13:
  every field of every row the same (`build/connectors/e2e-jdbc/Meta.java`,
  both outputs beside it).
- Found: getTables with no type filter crashed the node. Its query orders
  by TABLE_TYPE, a CASE's alias and also the view's column; the ungrouped
  ORDER BY's alias loop went on after a match and read the CASE's empty
  path. The gate held only the filtered form, where nothing followed the
  match. Fixed (the first alias, then stop, as the grouped paths do), the
  query put in the gate, watched failing under ASan first.
- Fuzz targets moved onto Dannan's harness the same day (d4c61e6): each
  runs in kvasir_tests too.
- Gate (a build node): release, clang, ASan, TSan each 393 tests and 966
  of 966.

## 2026-11-21: Step 3, parts 2 and 3: decided, and a proposer per range

- Part 2 (62a3690): the commit proved at `decided`. Acceptors answer from
  memory and sync now and then; a crashed one restarts not ready and
  catches up before it votes (Dannan's rule, confirmed with its session).
  A minority's crash loses nothing unexcused; a majority killed together
  stalls the range, never choosing twice. The first test killed a node
  while another was still catching up, so every run stalled; it now kills
  one only while the rest are caught up, as Dannan's runs do.
- Part 3 (3a5cd10): `kvasir/txn/range_proposer.h`, batching the commits a
  node is sent within a window, one change in flight. Every node proposing
  on one range at once: serializable, nothing lost or misapplied, batches
  shared. Its first version used a freed agreement after a node's crash;
  `stop()` now comes before the agreement goes.
- Gate (a build node): release, clang, ASan and TSan each 392 tests and
  965 of 965.

## 2026-11-21: Step 3, part 1: a commit on one range, proved in Dannan's simulator

- Written first: `TODO/2026-11-20-commit-protocol.md`.
- Landed (0516353): `kvasir/txn/range_commit.h`. A range is one name in
  Dannan's agreement; a batch of commits is one change, each commit checked
  against its rows' read versions and applied or refused whole, idempotent
  by commit id (Changed::repeatable).
- Proved: 40 seeds of Dannan's simulator, three nodes, messages dropped,
  duplicated and delayed, nodes killed singly and in pairs: serializable,
  nothing committed lost without a fault to excuse it, nothing refused
  applied; a change without the version check is caught.
- Found on the way: the struct was first named `Transaction`, the store's
  class's name; clang's build crashed on the one definition rule broken.
  Renamed `Commit`. The sanitizer steps' symbol check had read a stale
  library left by an earlier pin; it now reads what the tests load.
- Agreed: commits across ranges are Dannan's (item 26, 0.12.0); Dagda's
  durable AcceptorStore comes after its 0.4.0.
- Gate (a build node): release, clang, ASan, TSan each 390 tests and 965
  of 965; Dannan's and Kvasir's libraries carry the sanitizers' symbols.

## 2026-11-20: Release 0.1.0, one node in memory

- Kvasir's first release, on Dannan 0.9.2: one node, in memory, MySQL
  compatible as the README lists, at `ephemeral` only (the maintainer's
  ruling: a stable release keeps no promise it cannot keep).
- Coverage to 99.1% of lines (8,145 of 8,218), functions 98.0%, in six
  rounds of tests from the manual and the reference server; what they found
  is matched (be176c6). CI's tidy step checks use after move.
- Packaged as `kvasir` for Debian trixie on x86-64 and arm64, lintian clean
  (`docs/packaging.md`); CI fuzzes each target and builds the package.
- The gate (a build node, 2026-11-20): release, clang, ASan with UBSan, TSan
  (Dannan built from source, instrumented: `__asan_` and `__tsan_` imports
  in its library) and coverage each pass 380 tests and 962 of 962
  statements; the four fuzz targets ran 60 seconds each clean; a build
  against an installed Dannan passes the suite. CI green on 4a11b14.
- Next release: Dannan 0.10.0 and its DANNAN_SANITIZE option.

## 2026-11-20: information_schema's SCHEMATA, TABLES and COLUMNS (plan 1c)

- What JDBC's metadata calls read, observed on MySQL 8.4.13 first: each
  view's columns with their types, read directly and through a temporary
  table (ORDER BY, GROUP BY, DISTINCT), and Connector/J's full getTables
  and getColumns queries.
- Landed: `kvasir/exec/information_schema.h`, the three views built from
  the catalog when a statement names them and read with no transaction.
  - MySQL's other 79 views give 1235, an unknown name 1109.
  - Writes give 1044, before any lookup.
  - USE works, with utf8mb3 as the database's character set; SHOW DATABASES
    lists it.
  - TABLES' times are refused when named; its storage figures are NULL.
- Found and matched, each from a probe:
  - a view column is named as the view names it;
  - an expression is typed from the view's own column types, a bare column
    reported as the temporary table's field;
  - over a view with ORDER BY, an expression's integer is LONG below 10
    digits' length, its string has 0 decimals, any BLOB is BLOB;
  - utf8mb3_bin compares (as utf8mb4_bin) and is flagged BINARY;
  - CASE and its kin keep the largest BLOB kind among their operands;
  - UPPER and LOWER of a LONG_BLOB keep its length uncapped;
  - an integer column keeps its own length in a CASE.
- A crash fixed (in code before this feature): a grouped SELECT whose ORDER
  BY named an expression's alias, with another aliased item after it, read
  a path the expression had not. A new statement in the gate crashed the
  old build first.
- My errors: a probe database (`kv_is`) left on the reference server had
  leaked into one recording; dropped and re-recorded.
- Test-first on node-d: the 9 new statements failed (713 of 722)
  before the code.
- Gate: release on node-d **367 passed, 0 failed, 0 warnings**; the
  differential gate **728 of 728**. Before the merge, once each on
  node-c (`tools/ci.sh asan`, `tsan`), which also settles the run
  UPPER and LOWER owed: ASan with UBSan **367 passed**, 728 of 728, no
  report; TSan **367 passed**, 728 of 728, no report. Squashed onto
  develop as ee383be.

## 2026-11-20: HAVING without GROUP BY, in the gate

- JDBC's getTables filters with HAVING and no GROUP BY. Observed on MySQL
  8.4.13 first (13 statements: aliases, a CASE alias with IN and NULLs,
  columns in and not in the select list, WHERE with HAVING, no table, and
  an aggregate beside a bare column). Kvasir already answered every one as
  MySQL does (its grouping step), so no code changed and no sanitizer run
  was due.
- Gate: release on node-d (the build of 450b26f) **367 passed**;
  the differential gate **707 of 707** (13 new statements).

## 2026-11-20: UPPER, LOWER, UCASE and LCASE

- Observed on MySQL 8.4.13 first, for every code point: UPPER() and
  LOWER() dumped under utf8mb4_0900_ai_ci, utf8mb4_general_ci and
  utf8mb4_bin (2,621 code points changed by some collation, recorded as
  `kvasir/test/differential/casing.tsv`), with probes of the rest. The
  0900 collations use Unicode 9.0.0's simple mappings; every other
  collation (utf8mb4_bin and _general_ci, the utf8mb3 ones, and
  utf8mb4_unicode_ci as far as probed) uses Unicode 3.0.0's in the BMP;
  binary strings are unchanged. MySQL maps in place in a buffer of the
  argument's length, so a mapping that lengthens a character (U+023A to
  U+2C65) ends the result early: LOWER('Ⱥ') is '', LOWER('aȺb') 'aⱥ'.
  Results take the argument's type, never NOT NULL; 1582 for another
  argument count.
- Landed: Unicode 9.0.0's UnicodeData.txt in `kvasir/data/unicode/`;
  `tools/kvasir_casing.cpp` compiles both versions' case tables at build
  time; `kvasir/exec/casing.h`; the four functions. My first guess that
  utf8mb4_bin followed Unicode 9.0 was wrong; the dump showed it agrees
  with general_ci on all 2,621 code points.
- Known difference: utf8mb4_unicode_ci's mapping was probed for a few
  characters only.
- Test-first on node-d: the casing tests failed against a stub,
  the function tests and 3 new differential statements against the code
  before the functions, the rest passing.
- Gate: release on node-d **367 passed, 0 failed**; the
  differential gate **694 of 694** (10 new statements).
- Developed on `feature/upper-lower` and squashed onto develop, per the maintainer's
  direction (relayed by the coordinating session, 2026-11-20).

## 2026-11-20: Dannan's SteppedTime in the tests

- Released by the coordinating session as settled by the stack's goals: two
  tests defined their own stepped TimeSource; they now use
  `dannan/test/fixtures/stepped_time.h` (4ed4c06).
- Gate: release on node-d **362 passed, 0 failed**; the
  differential gate **684 of 684**.

## 2026-11-20: Dannan pinned at 0.9.2

- Most of the stack is on Dannan 0.9.2 (the coordinating session's audit,
  item 32). 0.9.1 and 0.9.2 fix how adverts reach peers, which Kvasir does
  not use yet; the ABI is 0.9.0's. Nothing in Kvasir changed.
- Gate: a full release build on node-d with Dannan fetched fresh at
  0.9.2 (3111ec3): **362 passed, 0 failed**; the differential gate **684
  of 684**.

## 2026-11-20: prepared statements, (d) real drivers; two sign-in fixes

- Ran against kvasir-node on node-d over TLS, and against the
  reference server, the same programs (`build/connectors/e2e-go`,
  `e2e-py`, `e2e-jdbc`): Go's go-sql-driver/mysql 1.8.1 (prepared on the
  server: a SELECT over a table, -5, 2.5, "ab", NULL, an unsigned BIGINT,
  a float, bytes, a bool, a 70000-byte string, INSERT, DELETE, a missing
  table, LIMIT ?, a transaction rolled back), mysql-connector-python 26.10
  prepared and plain, PyMySQL 1.2.4, and Connector/J 9.1.0 with and
  without useServerPrepStmts. Every answer from Kvasir is the reference
  server's. The throwaway node, certificate and accounts were removed.
- Found and fixed on the way, each test-first (watched failing on
  node-d):
  - The database a handshake names (CLIENT_CONNECT_WITH_DB, in Go's and
    JDBC's DSNs) was read and dropped: every statement on a table answered
    1046. The session now starts in it, and one that is not there is
    refused with 1049 in place of the OK (observed), the connection
    closed (7de1ae7).
  - Every Connector/J sign-in was refused: Connector/J reads the greeting's
    nonce as ASCII, and Kvasir's used whole bytes. MySQL's are 1 to 127
    and never '$' (observed over 300 greetings); Kvasir's now are
    (9610406).
- Gate: release on node-d **362 passed, 0 failed**; the
  differential gate **684 of 684**.

## 2026-11-20: prepared statements, (c) the command phase

- Landed: COM_STMT_PREPARE (the reply, parameters' and columns'
  definitions, each list ended by EOF unless CLIENT_DEPRECATE_EOF; 1064
  for a syntax error or a second statement; 1390 past 65535 placeholders),
  COM_STMT_EXECUTE (types kept per statement, binary rows, OK for a
  change; a lock wait runs it again with the same values; 1835 for bytes
  that do not fit, 1235 for a cursor), COM_STMT_SEND_LONG_DATA (never
  answered; past max_allowed_packet the execute is refused with 1153),
  COM_STMT_CLOSE (never answered), COM_STMT_RESET (OK; 1243 for an id not
  prepared). Questions leave out COM_STMT_PREPARE, CLOSE and RESET (the
  manual). The query and execute replies share one writer.
- Fixed with a capture: MySQL's server sends no NUM flag in a column
  definition (its client library adds it); Kvasir sent it in every text
  result set. The session's types keep it, as the differential gate
  records them through MySQL's client.
- Test-first on node-d: the 6 new command tests failed with the
  command codes and no behaviour, the 354 others passing. Every expected
  packet is MySQL's captured bytes.
- Gate: release on node-d **360 passed, 0 failed**; the
  differential gate **684 of 684**.

## 2026-11-20: prepared statements, (b) the session

- Landed: the parser numbers placeholders only for a prepared statement's
  text (a text query with `?` is 1064, as MySQL: 2 statements added to
  the gate); a placeholder is typed and read from its bound parameter;
  Session::prepare (MySQL's refusals at prepare, the parameters' types as
  MySQL infers them, a SELECT's result typed and stopped before any row,
  1461 past 16382 per session), bind (each binary type as MySQL reads it,
  dates and times refused with 1235), execute_prepared (as execute(),
  diagnostics included; LIMIT ? read from its parameter, 1210 for one
  that is not a count), close_prepared; COM_RESET_CONNECTION forgets
  them. Every expected type and value is from the captures.
- Found: MySQL's server sends no NUM flag (0x8000) in a column definition;
  its client library adds it. Kvasir's types carry it, and the command
  phase still sends it: to strip at (c), with a capture.
- Known differences: parameters are inferred from an integer column or
  literal beside them, LIMIT, and an INSERT's or UPDATE's column; MySQL
  infers more cases (a DECIMAL or DOUBLE column, functions' arguments).
  The prepared-statement limit is per session; MySQL's is per server.
- Test-first: not held for the session; its four tests were written
  before its code but could not compile without the new API, so their
  first run was with it. The parser's change was written before its
  test. The 2 differential statements failed against HEAD (1235 where
  MySQL says 1064) only by reasoning, not run. SqlParser.Literals printed
  a `?` from a text query; it now does so with placeholders allowed, as
  MySQL refuses it in text.
- Gate: release on node-d **354 passed, 0 failed**; the
  differential gate **684 of 684**.

## 2026-11-20: prepared statements, (a) the protocol's messages

- Captured first: what Go's driver and mysql-connector-python exchange
  with MySQL 8.4.13 for prepared statements, through a logging proxy
  (decoded in `build/connectors/capture/NOTES.md`, not in git).
- Landed: `kvasir/protocol/statements.h`: the COM_STMT_PREPARE reply, the
  COM_STMT_EXECUTE reader (untrusted: bounded by the parameter count and
  the packet, types kept from the last bind, long data in place of a
  value, a date's or time's bytes kept as sent), and binary rows from a
  result's text by column type; `kvasir/fuzz/execute_fuzz.cpp`. Not yet
  wired to a session or the command phase.
- Test-first on node-d: against stubs the first two tests failed
  and the third crashed (an empty vector read), so the stub run's count
  was not seen.
- Gate: release on node-d **349 passed, 0 failed**; the
  differential gate **682 of 682**. The fuzz target compiled under g++
  -fsyntax-only only; its first run waits for a 15 GB Pi with clang.

## 2026-11-20: the handshake's collation

- Found while capturing Go's prepared statements: Kvasir read the
  collation a client asks for in its handshake and ignored it. Observed on
  MySQL 8.4.13 (Go's driver through a capturing proxy, collations 45, 46,
  latin1, binary): the session starts as SET NAMES <its character set>
  COLLATE <it> would leave it, results reported in it. Go asks for
  utf8mb4_general_ci (45) by default, so its clients saw 255 where MySQL
  says 45. Now the session takes it at start and again at
  COM_RESET_CONNECTION; a character set no session speaks (latin1, which
  the `mysql` client sends in a POSIX locale, utf8mb3) leaves the
  server's, a known difference.
- Test-first on node-d: the new session test failed with the
  field and no behaviour.
- Gate: release on node-d **343 passed, 0 failed**; the
  differential gate **682 of 682**.

## 2026-11-19: result metadata in character_set_results

- Observed on MySQL 8.4.13 first (four probe rounds, written out with
  every observed value in ACTIVE.md as of 102492a and a78f54b, and in
  `reported()`'s comment):
  every string column has its own type and is reported converted to the
  results' collation (a BLOB's length times the results' widest
  character, another's characters times it); binary strings never, and
  nothing when character_set_results is NULL, as Connector/J sets it. The
  results' collation is SET NAMES' COLLATE, else character_set_results'
  default; collation_connection alone does not move it.
- Landed: the evaluator's types are each expression's own (system
  variables, session functions and SHOW's columns utf8mb3_general_ci,
  names utf8mb3_bin, table columns their own collation, CONCAT, the string
  and flow functions their aggregated collation, BLOB lengths unmultiplied);
  `reported()` makes what is sent, once, in Session::execute. Found on the
  way and fixed: after SET collation_connection = 'utf8mb4_bin' Kvasir
  reported results in utf8mb4_bin (MySQL: 255, BINARY on a literal); the
  length of CONCAT over a system variable counted utf8mb4 bytes where
  MySQL counts utf8mb3's (CONCAT('a', @@sql_mode): 262152, Kvasir said
  349536).
- The evaluator tests' helpers report under utf8mb4 results, as their
  expected types were observed; no expected value changed.
- Known difference: REPEAT of a utf8mb3 system variable past 16 MB is a
  LONG_BLOB in Kvasir; MySQL reports a MEDIUM_BLOB of 16777215 characters.
- Test-first on node-d: the 27 new differential statements, 11
  failing (671 of 682 with the superset rule in); the rest of the suite
  passing.
- Gate: release on node-d **342 passed, 0 failed**; the
  differential gate **682 of 682** (27 new statements; the 655 before
  recorded unchanged). clang-format clean.

## 2026-11-19: utf8mb4 outranks utf8mb3

- Found while writing the metadata step's tests: CONCAT of a utf8mb4 and a
  utf8mb3 column was refused with 1267. Observed on MySQL 8.4.13: between
  two IMPLICIT strings utf8mb4, the superset, wins, even over utf8mb3_bin
  (v = mb compares without case), in CONCAT, comparisons, LIKE, IFNULL and
  COALESCE. Kvasir now does the same; utf8mb3_bin is named in messages.
- Test-first on node-d: the new session test failed (1267).
- Gate: release on node-d, 342 tests: **341 passed**, and the
  differential test failed only on the metadata step's 11 statements, then
  uncommitted; the 655 committed statements all answered as MySQL does.
  (First recorded here as "341 passed, 0 failed", which was wrong.)

## 2026-11-19: what connectors send as they connect

- Logged on the reference server's general log, each connector run in a
  throwaway container on the Mac (Docker network `kvasir-probe`, account
  `probe`@'%', password in `build/mysql84/probe-user.pw`, scripts in
  `build/connectors/`): Connector/J 9.1.0 (its first query of 19
  variables behind a version comment, `SET character_set_results = NULL`,
  `SET autocommit=1/0`, COMMIT), mysql-connector-python 26.10.0, C and
  pure (`SET NAMES 'utf8mb4' COLLATE 'utf8mb4_0900_ai_ci'`, `set
  autocommit=0` or `SET @@session.autocommit = OFF`, commit), PyMySQL
  1.2.4 (`SET NAMES utf8mb4`, `SET AUTOCOMMIT = 0`), go-sql-driver/mysql
  1.8.1 (nothing of its own; START TRANSACTION, COMMIT).
- Every one of those statements already answered as MySQL does: 21 added
  to the differential gate (Connector/J's less @@performance_schema, an
  allowed difference), no code changed.
- Found by probing after `SET character_set_results = NULL`: MySQL
  reports each string column in character_set_results, converting the
  length; with NULL nothing is converted, so system variables and session
  functions show their own utf8mb3_general_ci (@@sql_mode 33/65535,
  DATABASE() 33/192). Kvasir reports them in the connection's collation.
  Not in the gate yet; the next step (ACTIVE.md).
- Gate: release on node-d **341 passed, 0 failed**; the
  differential gate **655 of 655** (21 new statements; the 634 before
  recorded unchanged).

## 2026-11-19: COM_STATISTICS, and the `mysql` client's `status` works

- Observed on MySQL 8.4.13 first: the reply is one string with no header,
  "Uptime: N  Threads: N  Questions: N  Slow queries: N  Opens: N  Flush
  tables: N  Open tables: N  Queries per second avg: N.NNN", the average
  Questions / Uptime cut (not rounded) to three places (2115 / 540 shown
  as 3.916). Questions counts a sign-in and every command but COM_PING and
  COM_STATISTICS (the manual's "Questions"; measured connection by
  connection: a refused statement counts, a failed sign-in does not).
  Threads is MySQL's Threads_running.
- Landed: `kvasir/server/statistics.h`, `NodeStatistics`, one per node
  (atomics, the injected steady clock), given to each connection's command
  phase; a command runs from its packet until the step that answers it, a
  statement waiting for a lock included. Kvasir has no table cache and no
  slow query log: Slow queries, Opens, Flush tables and Open tables are 0.
- Known difference: MySQL counts its event scheduler among Threads, so an
  idle MySQL says 2 where Kvasir says 1.
- Tried with MySQL 8.4.13's own client (Docker on the Mac) against
  kvasir-node on node-d over TLS: `status` filled every field
  (Db characterset included) and ended with the statistics line;
  `mysqladmin status` the same. The throwaway node, certificate and
  account were removed.
- Test-first on node-d: the 3 new tests failed against stubs, the
  338 others passing.
- Gate: release on node-d **341 passed, 0 failed**; the
  differential gate **634 of 634**. clang-format clean.

## 2026-11-19: character_set_database and collation_database

- The reference server is back, in Docker on the Mac at the maintainer's word
  (`community-server:8.4.13`, x86_64); the 605 statements re-recorded on
  it came back byte-identical. clang-format was installed on
  node-d at the maintainer's word, and a tree rebuilt there: **337 passed**
  before any change.
- The `mysql` client's `status` command, logged on the reference server,
  sends `select DATABASE(), USER() limit 1`, `select
  @@character_set_client, @@character_set_connection,
  @@character_set_server, @@character_set_database limit 1` and
  COM_STATISTICS. The second failed on Kvasir (1193); COM_STATISTICS is
  next.
- Observed on MySQL 8.4.13 first: both variables follow USE, are the
  server's with no database (and after dropping the current one), and
  DEFAULT for either restores the current database's pair; setting a
  character set brings its default collation and a collation its
  character set, each with 1681 "Updating '...' is deprecated. It will be
  made read-only in a future release." NULL is 1231 for every collation
  variable (Kvasir said 1232 for collation_connection; fixed).
- Known differences: a character set or collation Kvasir does not
  convert (latin1, utf8mb3) is refused with 1235 where MySQL takes it
  (with 1287 for utf8mb3), as for the connection's variables; a number
  (a collation id) is refused with 1232.
- Test-first on node-d: the new session test and the differential
  gate (611 of 634) failed against HEAD's code, the 336 others passing.
- Gate: release on node-d **338 passed, 0 failed**; the
  differential gate **634 of 634** (29 new statements; the 605 before
  recorded unchanged). clang-format clean on the changed files.

## 2026-11-19: handoff, after the build cluster was wiped

- 2026-11-18: Miach's work was given the cluster for the evening and
  Kvasir stood down; then all four Pis were wiped to a bare OS at the maintainer's
  word (both relayed by the coordinating session). Kvasir held no claim and
  had nothing running. Lost with it: every `/root/kvasir-work` tree and
  build, the fuzz corpora, and the MySQL 8.4.13 reference server with its
  password. Saved to `build/pi-rescue/` first: `probe.sh`, the probe SQL
  files, the collation checks and the container's image and port.
  Everything else is in git.
- The documents were brought into line for a session starting cold:
  ACTIVE.md (what the Pis now have, the order to start again, the known
  differences in one list, hosted CI without sanitizers), the README's
  status, build and layout, CONTRIBUTING's CI sentence, and the local
  notes.
- No code changed and nothing was built. The last gate stands (2026-11-17,
  node-c): release **337 passed, 0 failed**; the differential gate
  **605 of 605**. No sanitizer run. No claim is held.
- Resume from `TODO/ACTIVE.md`, "Resume here". The reference server must
  come back before SHOW STATUS can be started.

## 2026-11-17: hosted CI without sanitizers

- At the maintainer's word the ASan/UBSan and TSan jobs left `.github/workflows/ci.yml`
  (6eb0322), with the container options they needed. `tools/ci.sh asan`
  and `tsan` remain, for a release or a bug hunt.

## 2026-11-17: ABS, SIGN, FLOOR, CEILING, ROUND, TRUNCATE and MOD

- Observed on MySQL 8.4.13 first. An exact argument gives an exact
  result: ABS keeps an integer's length (as a BIGINT) or a DECIMAL's type
  (1690 naming 'abs(...)' for the least BIGINT); FLOOR and CEILING a BIGINT
  of 21 while 18 digits hold the result, else a DECIMAL; ROUND and
  TRUNCATE of an integer a BIGINT of 21, of a DECIMAL a DECIMAL at the
  constant decimals asked for (ROUND one digit wider when it drops any; the
  argument's own scale when the decimals are not a constant), half away
  from zero, negative decimals rounding to tens. Anything else is a DOUBLE,
  rounded half to even; NULL and a hex literal under ABS a DOUBLE of 17 and
  no decimals (observed). SIGN a BIGINT of 21. MOD(a, b) is parsed as
  a % b (1064 for other counts).
- Test-first on node-c: the 3 new tests failed before any code;
  one type (ROUND(1.234,5)) then corrected to the observed rule.
- Gate: release on node-c **337 passed, 0 failed**; the
  differential gate **605 of 605** (17 new statements; the 588 before
  recorded unchanged).

## 2026-11-17: TRIM, LTRIM, RTRIM, REPLACE, LOCATE, INSTR, POSITION, LPAD, RPAD, REPEAT and REVERSE

- Observed on MySQL 8.4.13 first. The string results hold their first
  argument's collation (no 1267 for mixed ones) and are never NOT NULL.
  TRIM and REPLACE match bytes (case counts); LOCATE, INSTR and POSITION
  compare under the strings' collation ('B' is found in 'abc'), bytes when
  the collations disagree, BIGINT of 11, an empty needle found at its
  position up to one past the end. REPLACE is reported as long as the
  string times one less than the replacement's length (at least once), as
  observed. LPAD and RPAD as long as the constant length asked for; REPEAT
  the string times the count; a negative constant reads as past any limit,
  and a length or count that is not a constant (or is NULL) gives a LONG
  BLOB of max_allowed_packet characters. A result past max_allowed_packet
  is NULL with 1301 "Result of repeat() was larger than ...".
- The parser takes TRIM([{BOTH | LEADING | TRAILING} [remstr] FROM] str)
  (`scope` holds the side) and POSITION(substr IN str), and gives REPLACE
  three arguments and REPEAT two, as MySQL's grammar (1064); LOCATE,
  INSTR, LPAD, LTRIM and the rest refuse other counts with 1582.
- Not yet checked against MySQL: the type of LPAD with a NULL length
  (Kvasir: a LONG BLOB, as for REPEAT with a NULL count).
- Test-first on node-c (node-d off the network): the 3 new tests
  failed against the committed code; node-c's tree first moved to Dannan
  0.9.0 and rebuilt in full.
- Gate: release on node-c **334 passed, 0 failed**; the
  differential gate **588 of 588** (19 new statements; the 569 before
  recorded unchanged).

## 2026-11-17: SUBSTRING, SUBSTR, MID, LEFT and RIGHT

- Observed on MySQL 8.4.13 first. In characters (bytes of a binary
  string); a position from 1, from the end when negative, 0 or past either
  end giving ''; a length of 0 or less ''. Arguments read as integers: a
  DECIMAL rounded half away from zero, a DOUBLE to nearest, a string by its
  leading digits with 1292 "Truncated incorrect INTEGER value". Never NOT
  NULL; as long as the string less what a constant position or length
  takes away (a column's takes nothing), sized as CONCAT's (a short piece
  of a TEXT is a VARCHAR). The parser takes `(str FROM pos [FOR len])` and
  refuses other argument counts with 1064, as MySQL's grammar does, MID
  included.
- Known difference: a constant string position such as '2x' warns once;
  MySQL warns twice (once typing, once evaluating).
- Test-first on node-d: the 3 new tests failed before any code.
- Gate: release on node-d **331 passed, 0 failed**; the differential
  gate **569 of 569** (7 new statements; the 562 before recorded
  unchanged).

## 2026-11-17: CONCAT, CONCAT_WS, LENGTH and CHAR_LENGTH

- Observed on MySQL 8.4.13 first. CONCAT is NULL when any argument is;
  CONCAT_WS skips NULL arguments and is NULL only for a NULL separator;
  both are never NOT NULL, in the arguments' aggregated collation (a
  number's the connection's at NUMERIC, NULL's binary at IGNORABLE; 1267
  for 'concat'), as long as their arguments' text together (a separator
  between each two, NULL or not). Past 65535 bytes the result is a MEDIUM
  or LONG BLOB whose text length is counted in characters once more, at
  most max_allowed_packet's default. LENGTH and OCTET_LENGTH count bytes,
  CHAR_LENGTH and CHARACTER_LENGTH characters (a binary string's bytes),
  BIGINT of 10. 1582 for a wrong argument count. In the new
  `kvasir/exec/functions.cpp`; the coercibility levels moved to
  expression.h.
- Test-first on node-d: the 2 new tests failed before any code.
- Gate: release on node-d **328 passed, 0 failed**; the differential
  gate **562 of 562** (15 new statements; the 547 before recorded
  unchanged).

## 2026-11-17: IFNULL, COALESCE, NULLIF, IF and CASE

- Observed on MySQL 8.4.13 first (four probe rounds, then the recording).
  The result type aggregates the operands', NULL aside: any string makes a
  string in the operands' aggregated collation (1267/1270 naming 'ifnull',
  'coalesce', 'if', 'case' or 'nullif'), as long as the longest operand's
  text (a DOUBLE's is 22), a CHAR only when every string is, a BLOB when
  any is; else any DOUBLE a DOUBLE; else any DECIMAL one wide enough for
  each; else the widest integer type, one wider where an unsigned type at
  least as wide meets a signed one, DECIMAL past BIGINT UNSIGNED. A column
  counts as an expression over it (`column_as_expression`, moved from
  aggregate.cpp for MIN and MAX). NULLIF reports its first argument, never
  NOT NULL. IFNULL and COALESCE show a DECIMAL at the result's scale, IF
  and CASE at the chosen value's own. Only what is needed is evaluated.
  CASE compares its operand with each WHEN under the strings' aggregated
  collation, or as numbers when any is not a string.
- IF takes exactly three arguments and COALESCE at least one, in the
  parser (1064, as MySQL's grammar); IFNULL and NULLIF two (1582).
- Found on the way, fixed with their observed tests: `_binary'ab'` was
  typed as a utf8mb4 string of 8 (MySQL: binary, 2), and a binary string
  other than a hex or bit literal was read as a big-endian integer in a
  numeric context (MySQL reads its text: `_binary'12'+1` is 13).
- Test-first on node-d: the 2 new tests failed before any code, the
  parser's one written with the code.
- Gate: release on node-d **326 passed, 0 failed**; the differential
  gate **547 of 547** (28 new statements; the 519 before recorded
  unchanged). No sanitizer run (not a release).

## 2026-11-17: aggregates, GROUP BY, HAVING and DISTINCT

- Observed on MySQL 8.4.13 first (three probe rounds, then the recording).
  COUNT, SUM, AVG, MIN, MAX (with DISTINCT), BIT_AND, BIT_OR, BIT_XOR,
  STD, STDDEV, STDDEV_POP, STDDEV_SAMP, VARIANCE, VAR_POP and VAR_SAMP,
  with MySQL's result types (SUM of an exact value 22 more digits, AVG
  div_precision_increment more digits and decimals, a string summed as a
  DOUBLE with 1292 per value; variance by Welford's recurrence, as MySQL).
- GROUP BY by column, expression, alias or position; groups in the order
  first met; HAVING over aliases, grouped columns and aggregates; one group
  for aggregates without GROUP BY (even of no rows). ONLY_FULL_GROUP_BY:
  1055 and 1140, a column fixed by a grouped key or by WHERE's
  `column = constant` allowed; without it, a group's first row. 1111 for an
  aggregate in WHERE or nested, 1056 for one in GROUP BY, 1054 in 'group
  statement' and 'having clause'.
- What a grouped result reports follows MySQL's temporary table: bare
  columns, bare COUNT/SUM/MIN/MAX and expressions over columns as the
  table's fields (no BINARY on numbers, 0 decimals on strings, MIN/MAX of a
  column as the column less NOT NULL and keys, GROUP_FLAG on a nullable
  grouped column); constants, AVG and expressions over aggregates as
  themselves; none of that when WHERE fixes the group or an aggregate is
  DISTINCT. A column beside aggregates without GROUP BY is nullable.
- SELECT DISTINCT was parsed and never applied: rows came back
  duplicated. Now each row once, reported through the temporary table
  unless the list holds a whole key (observed).
- Test-first: run on node-d with the AST fields and no code behind them;
  the 5 new tests failed, the 318 before passed. The DISTINCT metadata was
  found by the recording, after the code was written: its unit test and
  the differential statements came from MySQL's answers, not watched
  failing on their own.
- Gate: release on node-d **323 passed, 0 failed**; the differential
  gate **519 of 519** (86 new statements; the 433 before recorded
  unchanged).
- Correction: an ASan/UBSan run was started as part of this gate and
  stopped at the maintainer's word. Sanitizers run only before a release or to hunt a
  bug that cannot be pinned otherwise; the ASan counts recorded as gates for
  lock waits and UPDATE/DELETE were outside that rule.

## 2026-11-17: Dannan pinned at 0.9.0

- Dannan 0.9.0 tagged (main 59b7abc); the pin moved from 0.8.5. One
  break, in a test only: `server_pool_checker_test.cpp` built a
  `WorkerPoolConfig` by aggregate without its new `schedule` member,
  refused under -Werror=missing-field-initializers; given `{}` (threads,
  as before). Nothing in Kvasir's own sources changed.
- Gate: release on node-a **318 passed, 0 failed** (the
  differential gate within it, 433 of 433), the same count as before.

## 2026-11-16: UPDATE and DELETE

- Observed on MySQL 8.4.13 first: UPDATE counts changed rows, sends
  "Rows matched: M  Changed: C  Warnings: W", sets columns in the order
  written (each seeing those before), takes ORDER BY, LIMIT and an alias,
  and a raised AUTO_INCREMENT value carries the counter past it; DELETE
  removes row by row (ROW_COUNT() the count); their refusals are INSERT's
  (1048, 1062, 1364, 1366, 1406, 1054 in 'field list' or 'where clause').
- The store keeps deletions as versions, moves unique values with their
  rows (released at commit, judged with the transaction's own changes),
  and undoes a failed statement exactly (an undo log per statement).
  UPDATE and DELETE read the latest committed rows and the transaction's
  own (InnoDB's current read), so a concurrent committed change is built
  on, not lost; a row another transaction holds is waited for.
- Found while making it: DEFAULT before WHERE in UPDATE was read as a
  syntax error; DEFAULT is now the keyword unless "(" follows.
- Test-first: run on node-a against a stub store and HEAD's parser; the
  new tests failed (with the older row tests, the store being empty).
- Gate: release on node-a **318 passed, 0 failed**; ASan and
  UBSan on node-a **318 passed**; the differential gate **433 of 433** (51 new statements, recorded on
  node-b; the 382 before unchanged).

## 2026-11-16: lock waits

- A writer that meets another transaction's lock now waits, as InnoDB's
  do and the design says: the store registers the wait (who waits for
  whom), refuses one that closes a cycle (1213, 40001, the victim's whole
  transaction rolled back), and wakes waiters when the holder commits,
  rolls back or undoes a statement. The statement is undone and run again
  from the start once woken; nothing of it shows meanwhile. The command
  phase keeps it (`CommandStep::waiting`, `wait`, `resume`, `give_up`);
  the connection reads nothing while it waits and answers 1205 at
  innodb_lock_wait_timeout, the transaction kept.
- Found with MySQL's own client (kvasir-node on node-a, clients from
  node-b's container): a waiting connection was dropped at once, nothing
  holding it while no read was pending. A connection test now drops its
  own reference and waits (watched failing); the wait's timer holds the
  connection. After the fix, with the real client: a second writer
  waited about 3 s for a commit and got 1062; with
  innodb_lock_wait_timeout = 1 it got 1205 after about 1.3 s; after a
  rollback its insert went in. The throwaway node, certificate and
  account were removed.
- Test-first: the store's and command phase's tests were run against a
  store whose waits did nothing (every wait a deadlock, nobody woken) and
  failed, the 309 before them passing.
- Gate: release on node-a **312 passed, 0 failed**; ASan and
  UBSan (tools/ci.sh asan) on node-a **311 passed** before the
  connection test was added; the differential gate **382 of 382**. Note:
  that ASan run used all four of node-a's cores against a claim of two
  (ci.sh reads KVASIR_CI_JOBS, not JOBS).

## 2026-11-16: rows and transactions on one node

- Observed on MySQL 8.4.13 first (two probe runs): INSERT's refusals
  (1048, 1062 with MySQL's 'table.key' name, 1110, 1136 with the row,
  1264, 1265 as an error (01000), 1364, 1366 worded by type, 1406), its
  notes, the OK packet's "Records: N  Duplicates: 0  Warnings: W", how
  each type stores and is sent (ZEROFILL padded, FLOAT computing as the
  float it holds, CHAR without trailing spaces, BINARY padded with
  zeros), AUTO_INCREMENT (a statement reserves a value per row at its
  first need; a failed statement's are lost), key collisions under the
  key's collation, order without a primary key, NULLs in ORDER BY.
- Landed: `kvasir/txn/store.h`, a node's rows as committed versions, each
  transaction's writes buffered and locked by key and unique value,
  snapshots at the first read (REPEATABLE READ) or each statement (READ
  COMMITTED), a commit made visible at once, a failed statement undone
  alone, duplicates judged against the latest commit; `exec/rows.h`,
  stored values back as values and keys as bytes that sort and collide as
  MySQL's (PAD SPACE keys padded to the key's length); INSERT (VALUES,
  VALUE, SET, DEFAULT, column lists, multi-row) and SELECT over a
  table's rows (WHERE, ORDER BY by expression, alias or position, LIMIT,
  OFFSET); sessions hold transactions (BEGIN, COMMIT, ROLLBACK,
  autocommit = 0, DDL committing first, a session's end rolling back);
  LAST_INSERT_ID(). Interim, in the plan: a locked row answers 1205 at
  once instead of waiting; SERIALIZABLE is refused (1235); INSERT
  IGNORE, REPLACE, ON DUPLICATE KEY UPDATE, INSERT ... SELECT and columns
  in VALUES are refused (1235).
- Found by the gate: INSERT into a missing database is 1049, not 1146;
  arithmetic on an integer column counts the type's digits, not its
  display width. Both fixed, with unit tests.
- Test-first: the 11 new tests were run on node-d against an empty store
  and HEAD's parser and all failed, the 298 before them passing; then the
  code, which passed them at its first build.
- Gate: release on node-d (-j1) **309 passed, 0 failed**, the
  differential gate **382 of 382** (78 new statements, recorded on node-b;
  the 304 before unchanged). Hosted CI's sanitizers run on the push.

## 2026-11-16: tables in the catalog

- Observed on MySQL 8.4.13 first (six probe runs on node-b): column types,
  lengths, decimals and flags (PRI_KEY, UNIQUE_KEY, MULTIPLE_KEY, PART_KEY,
  NO_DEFAULT_VALUE; a UNIQUE key of NOT NULL columns promoted to primary),
  key naming (a, a_2, ... in the order written, a column's own keys among
  the table's), DEFAULT checks and notes, the 3,072-byte key and
  65,535-byte row limits (pack lengths, a byte per eight nullable columns),
  the engine (1286, or 1286 and 1266 without NO_ENGINE_SUBSTITUTION), the
  1681 deprecation warnings, and DROP TABLE all-or-none.
- Landed: `catalog/table.h` (column types, pack and key lengths, the
  primary key); the catalog holds tables (create, drop all or none, names in
  byte order, an id per table; DROP DATABASE takes its tables and counts
  them); the parser reads CREATE TABLE (MySQL's integer, decimal, float and
  string types and their aliases, column attributes, PRIMARY/UNIQUE/INDEX
  keys, table options), DROP TABLE(S), SHOW [FULL] TABLES, and marks valid
  MySQL not yet carried out (temporal, BIT, ENUM, SET, JSON and spatial
  types, CHECK, FOREIGN KEY, generated columns, DEFAULT (expression),
  LIKE, ... SELECT, TEMPORARY, partitioning, other table options) for a
  1235; `exec/ddl.h` makes MySQL's checks (1050, 1059, 1060, 1061, 1063,
  1067, 1068, 1070, 1071, 1072, 1074, 1075, 1089, 1101, 1103, 1115, 1118,
  1166, 1170, 1171, 1253, 1273, 1280, 1286, 1425, 1426, 1427, 1439) and
  reports a column's result type; `exec/store.h` stores a value as a column
  does under strict mode (DEFAULT now, INSERT next); SELECT over one table
  gives its columns' definitions (schema, table, alias, original names) and
  types expressions over them, with no rows yet. utf8mb3 columns are held
  (utf8mb3_general_ci, utf8mb3_bin) though no session speaks utf8mb3.
- Found by the gate: arithmetic on an INT column was typed DOUBLE (the
  evaluator knew only BIGINT as an integer type), fixed.
- Test-first: the 19 new tests were run against empty stubs on node-b and
  all failed, the 279 before them passing; then the code.
- Gate: release on node-d (-j1; -j2 ran out of memory) **298
  passed, 0 failed**, the differential gate **304 of 304** (117 new
  statements, recorded on node-b; the first 187 unchanged). Sanitizers: the
  Pis were all held; hosted CI's run on the push.

## 2026-11-16: the sign-in back-off, on Dannan 0.8.5

- Dannan 0.8.5 (de631cc) carries Lugh's Backoff as
  `dannan::listen::Backoff`; the pin moved. The sign-in asks it per
  principal and per source before anything is hashed (fast path included);
  a waiting client is denied as a wrong password is, without counting
  another failure; wrong and empty passwords count against both; a sign-in
  clears only its principal's. The node uses Lugh's rules (5 and 20 free,
  1 s doubling to 15 min, forgotten after an hour).
- Tried with the real mysql client: five wrong passwords, then the right
  one denied, then accepted after the second's wait.
- Test-first: the four tests were written before the code, but not run
  before it; the first build failed on the test helper (a const move-only
  step returned), fixed.
- Gate: release on node-d **279 passed, 0 failed**, the differential
  gate **187 of 187**.

## 2026-11-16: the node serves MySQL clients

- Dannan 0.8.4 tagged (52fd53c) with `Stream::start_tls_unanswered`; the
  pin moved. Landed: `StreamTransport` (a connection over an accepted
  Stream, TLS begun after exactly the SSLRequest), `PoolPasswordChecker`
  (Argon2id on a WorkerPool as foreground work with a deadline, owning the
  Secret, answering on the connection's executor; false when refused or
  dropped), and `kvasir-node` listening (TLS required, SIGINT/SIGTERM stop).
- Tried by hand on the Pis: kvasir-node on node-d, MySQL 8.4.13's own
  `mysql` client from node-b's container with --ssl-mode=REQUIRED:
  signed in over TLS, ran expressions, collations and LIKE, created, used
  and dropped a database, read @@ack_level; a wrong password got 1045, a
  plaintext attempt 3159; the node stopped cleanly on SIGTERM. The
  throwaway certificate and account were removed afterwards.
- Test-first: the checker's two tests were written with it, not watched to
  fail first; the transport is proven by the real client, with no unit
  test of its own.
- Gate: release on node-d (-j1) **275 passed, 0 failed**, the
  differential gate **187 of 187**.

## 2026-11-16: the catalog's databases

- Landed: `kvasir::catalog::Catalog` (in memory, one lock, names as
  bytes), shared by a node's sessions through their constructors and the
  connection's; CREATE / DROP DATABASE and SCHEMA with MySQL's options and
  refusals (1007, 1008, 1044, 1059, 1102, 1253, 1273, and 1235 for a
  character set Kvasir does not convert); SHOW DATABASES and its LIKE
  (case-sensitive, observed); USE and qualified table names checked against
  the catalog; dropping the current database leaves none current.
- Found by the gate: its own flag printer lacked NO_DEFAULT_VALUE.
- Test-first: the parser, session and gate tests failed first (3 failed,
  the gate 166 of 187); the catalog class itself was written beside its
  test, not watched to fail first.
- Gate: release on node-b **273 passed, 0 failed**, the
  differential gate **187 of 187**. Hosted CI passed every job on 0eb9efe.

## 2026-11-16: COLLATE, and collations aggregated as MySQL does

- Landed: COLLATE (checked against the operand's character set: 1253 for
  a mismatch or a character set Kvasir does not convert, 1273 for an
  unknown name; the result in the results character set, BINARY for a _bin
  collation, never NOT NULL); collation aggregation over every operand of
  a comparison, IN, BETWEEN and LIKE, with 1267 and 1270 naming the
  operation. COLLATE BINARY is a syntax error, as in MySQL.
- Found on the reference server and corrected: the earlier rule let a
  binary string outrank everything, but an explicit collation outranks it
  (X'41' = 'a' COLLATE utf8mb4_general_ci is 1); the _bin tie-break holds
  below EXPLICIT only.
- Test-first: 2 failed first (the COLLATE suite and the gate, 150 of 163).
- Gate: release on node-b **268 passed, 0 failed**, the
  differential gate **163 of 163**. Hosted CI passed every job on 2f13512
  and 5f26132 (string comparison and general_ci).

## 2026-11-16: LIKE under a collation

- Landed: `Collation::like`, one character at a time, each pair compared
  under the collation by sort key, never padded, a byte at a time under the
  binary character set; ESCAPE (one character, none when empty, 1210
  otherwise); numbers matched by their text. Every expectation observed on
  the reference server, including two ESCAPE '' answers reasoned first and
  then confirmed.
- Found by the differential gate: MySQL holds generated column names in
  utf8mb3, so a character past the BMP in one becomes '?'; fixed with a
  test. An alias holding one is refused by MySQL (3854) and still accepted
  by Kvasir: recorded in the plan.
- Test-first: against a stub, 3 failed (both LIKE suites and the gate, 132
  of 140).
- Gate: release on node-b **267 passed, 0 failed**, the
  differential gate **140 of 140**.

## 2026-11-16: the general_ci collations, derived from Unicode 3.0.0

- Ruling 33 (the maintainer, "Derive from Unicode"): Unicode 3.0.0's UnicodeData.txt
  checked in; `kvasir-general-ci` derives one weight per BMP code point.
  The rule was found by checking candidates against all 63,488 BMP weights
  dumped from the reference server (the dump stays on node-b, used only to
  verify): Unicode 3.0.0 matches best of 3.0.0 to 9.0.0; accents are
  stripped by canonical decomposition, never through a singleton, only in
  Latin, Greek and Cyrillic and only to a letter; then uppercase. Three
  exceptions recorded: sharp s weighs as S, and short i keeps its breve.
  The generated table equals MySQL's at every one of the 63,488.
- Found and fixed: PAD SPACE was implemented as trimming trailing spaces,
  so 'a\t' < 'a' under utf8mb4_bin answered wrongly (in 528b5a4); the
  shorter string now compares as padded with spaces. utf8mb4_bin's sort key
  is now three bytes per code point, as MySQL's.
- Test-first: 4 failed first (three collation tests and the gate, 124 of
  128). One expected key in the new test was wrong (U+0459 written for
  U+0451) and was corrected to the observed value.
- Gate: release on node-b **265 passed, 0 failed**, the
  differential gate **128 of 128**.

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
  its fuzz harness (taken as a 0.9.0 item). Gave CodeQL `actions: read`.
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
- Joins, J1 (the maintainer's choice, 2026-11-23): the FROM clause read
  into a join tree (JOIN, INNER, CROSS, STRAIGHT_JOIN, LEFT and RIGHT
  [OUTER], NATURAL, ON, USING, parentheses); the executor still refused
  joins (1235).
- Gate (node-c): release, ASan and TSan each **421 tests, 1165 of 1165**
  statements, TSan with no reports.
- Joins, J2: inner joins carried out on the fixed plan (nested loops in
  written order, each ON over its own tables); 1052, 1054, 1066 and 1051
  as MySQL answers them; `*`, `t.*`, WHERE, ORDER BY, DISTINCT and LIMIT
  over joined rows. Key flags: sorted by the first table alone, kept; by
  another, or DISTINCT, through a temporary table (recorded difference:
  MySQL's optimizer picks its own first table).
- Gate (node-c): release, ASan and TSan each **422 tests, 1188 of 1188**
  statements, TSan with no reports.
- Joins, J3: LEFT and RIGHT [OUTER] JOIN, USING and NATURAL carried out
  (NULL-extended rows, the inner side's NOT NULL dropped, USING's columns
  once and first, 1054 'from clause'). The temporary-table rule refined to
  the observed one: sort keys of the plan's first table and each later
  table found through an index keep key flags; else a temporary table.
  CI green on J1 (057413f) and J2 (5c471a5).
- Gate (node-c): release, ASan and TSan each **423 tests, 1207 of 1207**
  statements, TSan with no reports.
- Joins, J4: grouping over joins (one column interface for a table or a
  join; the join's construction shared); COUNT over the NULL side 0;
  ONLY_FULL_GROUP_BY through keys and the join's equalities; grouping off
  an index only when the plan's first table is grouped by one and each
  later table is found through an index. CI green on J3 (ef1747a).
- Gate (node-c): release, ASan and TSan each **424 tests, 1218 of 1218**
  statements, TSan with no reports.
- Joins, J5: locking reads over joins, plain and grouped (each table under
  its clause read as a locking read, the rows WHERE kept locked and its
  key range; OF by alias, 3568, 3569 in backquotes when OF named it);
  SERIALIZABLE and INSERT ... SELECT over joins share locks. Red first: 6
  of the new gate statements failed on the old code. CI green on J4
  (1bd90fd). Joins J1 to J5 done; multi-table UPDATE and DELETE left.
- Gate (node-c): release, ASan and TSan each **425 tests, 1224 of 1224**
  statements, TSan with no reports.
- Multiple-table UPDATE and DELETE (the last join stage): both forms of
  DELETE, UPDATE over references; each changed row once; changed tables
  locked exclusive, the others shared; 1109, 1221, 1052 as MySQL answers.
  CI green on J5 (512dd2e), but coverage fell to 98.7% of lines with the
  join code: the next step covers it.
- Gate (node-c): release, ASan and TSan each **427 tests, 1250 of 1250**
  statements, TSan with no reports.
- Join coverage: statements over what the join code left untested go into
  the gate (out-of-range naming joined columns, 3065, ORDER BY alias,
  position and unknown name, stars beside aggregates, an unknown database,
  an ambiguous grouped WHERE, a strict multi-table UPDATE); unit tests for
  a locking read over a join that must wait. Found: ORDER BY
  99999999999999999999 crashed over a join and was 1054 over one table; an
  integer literal past 64 bits is now a constant, not a position. Coverage
  99.2% of lines again.
- Gate (node-c): release, ASan and TSan each **427 tests, 1268 of 1268**
  statements, TSan with no reports.
- Temporal types, T1a (the maintainer's choice of temporal types next,
  2026-11-23): CAST and CONVERT, which Kvasir lacked, to SIGNED, UNSIGNED,
  CHAR, BINARY, DECIMAL, DOUBLE, REAL and FLOAT, with MySQL's warnings
  (1292, 1105, 1264) and errors (1425, 1426, 1427); other character sets
  and the temporal targets refused until built. Coverage 99.2% of lines.
- Gate (node-c): release, ASan and TSan each **429 tests, 1289 of 1289**
  statements, TSan with no reports.
- Temporal types, T1b part one: DATE, TIME and TIMESTAMP literals, and CAST
  and CONVERT to DATE, TIME and DATETIME, on a new module that reads,
  checks, rounds and writes dates and times as MySQL does
  (TODO/2026-11-23-temporal-notes.md); comparison as moments, a TIME beside
  a string as strings, arithmetic on their numbers; a failed statement
  lists its earlier warnings before its error. CI green on CAST (2f05fd8).
- Gate (node-c): release, ASan and TSan each **433 tests, 1321 of 1321**
  statements, TSan with no reports; coverage 99.1% of lines.
