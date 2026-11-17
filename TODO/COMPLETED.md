# Kvasir - Completed

The record, newest first. Each entry says what landed, with its gate counts.

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
