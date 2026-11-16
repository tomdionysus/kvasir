# Kvasir: design (2026-11-15, second draft)

Kvasir is a MySQL-compatible, multi-master, leaderless distributed relational
database with ACID transactions: a hosted system on Dannan, persisting in
Dagda, written new from this design. It is Mimir's sibling and shares its
concepts.

This draft replaces the first, written the same day. It is built from the maintainer's
rulings, the session's own review and the advice of eleven sibling sessions.
`2026-11-15-design-review.md` is the record of that review and advice and is
not revised further; the reconciliation that followed it is folded into this
document (its own file is in the git history). Each statement is marked:

- **Ruled**: the maintainer's decision, to this session, 2026-11-15, in his words where
  quoted.
- **Relayed**: the maintainer's decision to another session, as that session reported
  it; not heard by this session from the maintainer.
- **Proposed**: the Kvasir session's recommendation, not built until the maintainer
  rules and the owning session has reviewed it.

Of this design only the skeleton and the start of step 2 (the MySQL wire
codec) are built; `ACTIVE.md` says how far. The cluster design has been read
against Dannan's agreement design, not run.

## Ruled

1. Kvasir is "a sibling project to Mimir, using the same concepts and the
   standard style/deps (Dannan, Dagda, Lir, Lugh, etc)".
2. It is "a SQL database, MySQL client compatible".
3. It is "similar in concept and resilence to AWS's Aurora".
4. "The usual apply - leaderless, soft cluster, replicated, multimaster."
5. "It must offer ACID the same as other major databases."
6. "As an extension, Use Mimir's 3 level ack persistence per command or
   session (switchable)."
7. The goal: "What we want is MySQL but fast, replicated, storage scalable
   like Aurora, and even with the possibility of sharding work across the
   cluster. In short, a soft cluster MySQL that's damn near impossible to
   properly take down with infra failures. The design will be a compromise,
   and that's where what is useful comes in."
8. The levels, in his words: "0 - node returns immediately and the commit is
   actioned async. 1 - node returns once local persistence acks. 2 - node
   returns once majority all replication persistence acks. The intent is 0 -
   ephemeral/disposable data, 1 - persisted, 2 - replicated."
9. At the second of those the cluster's decision is async, as at the first:
   the node answers from its local disk and the commit can still be refused.
10. At the first, the node makes its local checks before it answers.
11. The default level is set in YAML and has no built-in value; a node
    without it refuses to start.
12. A commit that is answered and not yet decided is seen only by its own
    session.
13. A commit refused after it was answered is rolled back whole, counted on
    Status, recorded in a system table the application can query, and the
    session's next statement carries a warning.
14. On a minority side, a commit answered early is accepted, held and decided
    at heal; a commit that waits for a majority is refused there.
15. A commit waiting for a majority's disks always wins against another
    node's answered, undecided commit on the same row.
16. A sticky proposer is leaderless: each range's commits go to whichever
    node committed on it last; any node can take the range at any instant;
    no term, timer, election or failover procedure. Timed read leases were
    not chosen.
17. A fourth level is added: decided by a majority in memory, then answered,
    disks after. A per-table lowest level was offered and not chosen.
18. The second writer of a row waits, as InnoDB does.
19. The deterministic simulator is built once, in Dannan, proposed jointly
    with Mimir.
20. The tagline is "Kvasir - The Distributed Relational Database".
21. The compatibility reference is MySQL 8.4 LTS, and the differential gate
    runs against Oracle's MySQL 8.4 server.
22. Kvasir commits when a step's gate passes and pushes `develop` to a
    private remote, which the maintainer creates. (the maintainer made it public, 2026-11-15:
    "You can make your remote repo public, and push to it.")
23. The levels are named, not numbered, as a group standard: `ephemeral`,
    `persisted`, `decided`, `replicated`, set by `ack_level`
    (`2026-11-15-ack-levels-standard.md`). Dagda changes to the standard's
    meanings. Dagda's `durable` does not join: the level is the least the
    node waits for, the rest catch up, and the evidence says who had it.

24. Replication "should be completely configurable, in both replicas and
    domains. It should handle a single node, no replicas, and not fail when
    there aren't enough nodes for the replicas."
25. Reads that might be stale are served only to a session that opts in.
26. Before persistence exists, Kvasir runs and says it is not durable.
27. `decided` wins as `replicated` does (ruling 15).

28. A range with two replicas: configurable, and by default both are voters,
    so writes that wait for a majority pause while either is lost. The other
    setting is one voter and one following copy, whose promotion is an
    explicit operator action.
29. The replica count and domain spread have a cluster default and are
    overridable per table; the same setting drives Dagda's copies of that
    table's chunks.
30. The MySQL 8.4 server for the differential gate is Oracle's container on
    one build Pi, started by hand on a private port, after a note on the
    claim board.
31. Step 2a starts.
32. Collations (2026-11-16, chosen from the options put): "Unicode's data".
    Unicode's allkeys.txt for UCA 9.0.0 is checked into Kvasir as data, with
    Unicode's license; a generator written here compiles it into a weight
    table at build time; the algorithm is written by hand; nothing of MySQL
    is copied; the differential gate checks the results.
    utf8mb4_0900_ai_ci first, then _as_cs, _bin and general_ci.
33. general_ci (2026-11-16, chosen from the options put): "Derive from
    Unicode". utf8mb3_general_ci's and utf8mb4_general_ci's weights are
    derived by hand from Unicode's UnicodeData.txt (checked in as data, with
    its license): accents stripped by decomposition, then uppercased. All
    65,536 BMP weights are verified against the reference server's
    WEIGHT_STRING, and where the rule and MySQL differ the exceptions found
    are recorded. MySQL's table is not dumped or copied.

## Relayed

- By Dannan: anything that can use consensus or agreement waits for Dannan's
  agreement primitive; no consensus, lease standing in for it, or workaround
  is built meanwhile. Agreement is on Dannan's `develop` as 0.9.
- By Dannan: the simulator is built within 0.9.0, beside agreement, reviewed
  by Kvasir and Mimir. Its first pieces are on Dannan's `develop`.
- By Dannan: "No root except at genesis. A root may start with only one
  node." A fixed slot table; every slot touched at founding; a slot's set
  recorded in its own acceptors and moved only by `transfer()`; a hint of
  which slots a node serves carried in membership.
- By Dagda: `persisted` is "the disk that took it": this node's when it can
  hold the copy, else the next in placement order. A level says how far the
  write has gone, not where.
- By Mimir (its ruling 23): Mimir's agreement commands decide, then answer,
  at every level. By design unlike Kvasir's `ephemeral` and `persisted`.

## What the goal is taken to mean (Proposed)

- **MySQL**: the wire protocol and the SQL that real drivers, ORMs and tools
  issue, behaving as the MySQL 8.4 manual documents. A defined subset first.
- **Hard to take down** comes before fast. The failures the sessions saw on
  the hardware are brownouts under load, nodes that are slow and not dead,
  Wi-Fi at about 0.5 MB/s between Pis, partial partitions in which two nodes
  both believe they act, and clocks 5 to 15 ms apart with no bound.
- **Fast** is relative to that. A `replicated` commit costs at least one
  round trip to a majority and one sync on each. Speed comes from batching
  many commits into one decision, reading locally where that is sound, and
  the weaker levels, never from skipping the majority.
- **From Aurora**: only the log is written at commit; the stored form is
  built from it in the background; storage is replicated across failure
  domains and heals itself; a node's SQL state is soft; restore to a point
  in time and clones come from immutable storage. **Not from Aurora**: the
  single writer.

## Shape (Proposed)

- **One process per node**, `kvasir-node`, advertising capability `kvasir`.
  Every node serves every statement. A node need not hold data: placement is
  by capacity, so a small node can serve SQL and hold few ranges or none.
- **Clients** speak the MySQL client/server protocol (protocol 10) over
  `dannan::listen`. The packet reader, the lexer and the parser each have a
  fuzz target. A per-client outbound bound closes a slow client. MySQL has
  no redirect: clients are given several hosts or one name over all nodes; a
  connection whose node dies is lost with its session state and reconnects.
- **Ranges.** A table's rows are ordered by primary key and split into
  ranges; each secondary index is a keyed structure split the same way.
- **Replicas** (rulings 24, 28, 29). The replica count and the spread across
  failure domains are settings, with a cluster default in YAML and an
  override per table; the same setting drives Dagda's copies of that table's
  chunks. A range's set is as many data-holding nodes as exist, up to the
  count: one node alone is a working database with no replicas. A range
  short of its count works, is shown as under-replicated on Status, and
  grows by `transfer()` as nodes join. With two replicas both vote by
  default, so writes that wait for a majority pause while either is lost;
  the other setting is one voter and one following copy, promoted only by an
  explicit operator action. A failure domain is a power supply as much as a
  host. Proposed: the default count is three.
- **One truth.** A range is one register in Dannan's agreement: its value is
  the range's state, and each change is a batch of commits carried as a
  delta. Each replica's acceptor state, which holds the decided commits, is
  durable on that replica in a node-local Dagda log with no rollback. A
  commit is ordered and replicated once, by agreement, and nowhere else.
- **The sticky proposer** (ruling 16). Nodes forward a range's commits to
  the node that committed on it last, which keeps its ballot (Dannan's
  one-round fast path) and batches what arrives within the store's 10 to
  20 ms group commit into one decision: one round trip and one sync per
  replica per batch. Any node takes a range over by out-balloting, at two
  round trips once. A slow proposer is taken over from as a dead one is,
  with backoff so a range is not traded back and forth. Safety rests on
  ballots alone.
- **The range map** (which ranges exist, their bounds, sets and versions)
  lives in Dannan's slot registers, keyed by table. A range is first touched
  only after a committed map entry names its set. A move is a map change and
  a `transfer()`, with the new replicas caught up beforehand from the
  manifest and the log tail; a split or merge is an intent in the map, one
  change on the range's own set, then the map marked done, and whoever finds
  an unfinished intent finishes it. A stale map is corrected by a replica's
  refusal, which names the set now served.
- **Work classes.** A client's statement is foreground; replication a commit
  waits on is foreground; a replica catching up, and a large scan, are
  loader; folding, old-version collection, index builds, statistics and
  backups are speculative, paced by signal and never gated on idle.
- **Memory.** The data does not fit in memory. A node keeps a bounded cache
  and the unfolded tail of its ranges, declared in its `Budget`; a statement
  that cannot reserve what it needs is refused, never grown without bound.

## Transactions (Proposed, on rulings 5, 16 and 18)

- **The coordinator** is the node the client is connected to. It buffers the
  transaction's writes and holds nothing another node cannot finish or
  abort.
- **Locks and read marks live in memory at the range's proposer.** A writing
  statement takes its row locks there with one forwarded call and no
  agreement round. A second writer waits (error 1205 at
  `innodb_lock_wait_timeout`, 1213 on a cycle). `SELECT ... FOR UPDATE` and
  `FOR SHARE` lock the same way. The lock table is soft: when a range changes
  proposer, transactions in flight on it get 1213 and the client retries.
- **Commit.** On one range: the transaction's writes join the proposer's next
  batch. On several: one transaction record, a register on the first written
  range, decides every range's part at once; each range's part is prepared in
  its own batch; whoever meets an unfinished record resolves it (discipline
  3). Mimir's atomic multi-key commands across slots need the same piece; it
  is to be written once, in Dannan or beside it.
- **Constraints** (`NOT NULL`, `CHECK`, primary, unique, foreign key) are
  checked inside the commit's decision. A unique index is its own keyed
  structure, so two inserts of one value contend for one range.
- **Time.** Dannan's hybrid clock is used as a logical order only; no
  correctness rests on wall clocks. A proposer never commits at or below a
  timestamp it has served a read at, and tells readers its floor, so a
  snapshot over several ranges is consistent with no bound on skew. A session
  carries its own last commit timestamp forward, so it always sees its own
  commits; a transaction may not see another session's commit that finished
  milliseconds earlier on another node.
- **Reads.** A read that must be current goes to the range's proposer, which
  confirms it still holds its ballot with one majority round. A read that
  might be stale is served only to a session that has opted in (ruling 25),
  for autocommit `SELECT`: it takes a slightly old snapshot from any replica
  with no round, and is what a minority side can still serve.
- **Isolation.** `REPEATABLE READ` (the default) is one snapshot per
  transaction; `READ COMMITTED` one per statement; `READ UNCOMMITTED` is
  served as `READ COMMITTED`; `SERIALIZABLE` adds range read marks at the
  proposer, which stops phantoms.
- **`AUTO_INCREMENT`.** A node reserves a block of values by one agreed
  change: unique, increasing on a node, with gaps. A restore or clone moves
  every allocator past anything the original could have issued before it
  takes a write.
- **DDL.** The catalog is agreed state with a version; a commit is refused if
  the tables it wrote changed shape since. DDL commits implicitly, as in
  MySQL. An index build on a populated table is loader work with a visible
  state.
- **"Is this committed?" always has an answer.** Every transaction has an
  id. When a `COMMIT` cannot be answered in time the client is told the
  outcome is unknown, not that it failed, and can ask by id afterwards.

## Acknowledgement levels (rulings 6, 8 to 15, 17, 23)

`ack_level` is a session variable, a per-statement hint
(`COMMIT /*+ SET_VAR(ack_level='ephemeral') */`) and a YAML key with no
built-in default. The level in force at `COMMIT` covers the transaction.

| Level | The node answers | Decided by the cluster | Refused after the answer |
|---|---|---|---|
| `ephemeral` | after its local checks | after | possible |
| `persisted` | once one node's disk has it | after | possible |
| `decided` | once a majority has agreed it, in memory | before | never |
| `replicated` | once a majority has it on disk | before | never |

- At `replicated` a transaction is ACID as ruling 5 says. `decided` gives up
  only durability against a majority losing power together. At `ephemeral`
  and `persisted` the answer means accepted, not committed.
- At every level the database never shows part of a transaction or a broken
  constraint: nothing is visible to another session until the cluster has
  decided it (ruling 12), and a refusal is whole (ruling 13).
- **A minority side** (ruling 14) refuses `decided` and `replicated` and
  holds `ephemeral` and `persisted` commits for heal.
- **`decided` and `replicated` win** against another node's answered,
  undecided commit on the same row (rulings 15 and 27). With a sticky
  proposer this is its ordering rule, and it can hold only until the other
  commit is chosen: a priority, not an absolute.
- **Proposed, following from the rulings**: a session's `decided` or
  `replicated` commit first waits for its own held commits; held commits are
  decided in the order made; each carries an id (node, incarnation,
  sequence), and decided and refused ids are kept for a stated window so a
  commit put again after a heal or restart is never applied twice; only its
  origin, or the range's proposer once the origin has left the roster,
  finishes it; the hold is bounded and gives way in a stated order (shed
  `ephemeral`, refuse `persisted`, refuse connections, never crash); the
  table of refusals is `kvasir.refused_commits`, bounded.
- **What cannot be done**, said plainly to users: an `ephemeral` commit held
  on a node that dies is lost with nothing to record it from; and "its own
  session" does not span a connection pool, so an application that writes on
  one connection and reads on another will not see an undecided commit.
- **What they are good for**: `ephemeral` for telemetry, caches and session
  data; `persisted` and holding through a split for insert-only data with
  keys the node can mint. Updates to shared rows made against stale data are
  mostly refused at heal.

## Storage (Proposed; waits on Dagda's node-local log)

- **At commit**, only the acceptors' node-local logs are written.
- **In the background**, each replica folds decided commits into immutable
  chunks of a few MiB, stored as Dagda objects with claims. One head space
  holds every range's manifest (its list of chunks and the position they
  cover), committed once per fold and never per SQL commit: a head commit
  manages under a hundred a second per node, and a node may advertise only
  64 spaces.
- **Reads** take a row from the cache, else the chunks, else the unfolded
  tail. Old versions are dropped in folding once older than every open
  snapshot and the restore window.
- **Restore to a point in time and clones** are claims on the same chunks,
  taken before the source can release them.
- **Healing.** A replica that is behind takes the manifest and the tail from
  a peer; Dagda's repair restores lost copies of chunks; a failed sync
  latches and the replica re-derives; node-local state is never restored
  from an older copy. A range whose majority is lost for ever is rebuilt
  from a surviving replica and its chunks as an explicit, recorded operator
  action; Dannan offers no forced transfer.
- Every byte goes through Dagda, sealed. Kvasir writes as its node principal.
- **Before persistence exists** (ruling 26) the cluster runs in memory and
  says it is not durable: in its version comment, in a warning on a
  connection's first statement, and on Status.

## Built to stay up (Proposed)

- Slow is a failure: health by latency and progress, not heartbeats;
  under-voltage and throttling read from the node and shown on Status.
- A deadline on every call; refusal at the edge and backpressure by work
  class; no unbounded queue. An answer that involved more than one node says
  which answered and what failed first.
- Recovery is paced and budgeted so healing cannot brown out the survivors.
- A node comes back by being restarted, with no manual repair.
- Fence by ballot and generation, never by "it stopped answering".
- Formats on disk and wire are versioned, old ones read and unknown ones
  refused; an upgrade is one node at a time, tested from the previous
  release.
- Backup and restore to a point in time come with persistence, before any
  further replication feature.

## Proving it (Proposed)

- SQL behaviour is judged against a MySQL 8.4 server; the commit protocol's
  histories against a sequential model. Never against Kvasir's own output.
- The allowed differences from MySQL (row order without `ORDER BY`,
  connection ids, timings) are written down, and the gate compares up to
  that list.
- The protocol's invariant, stated before code: a replica equals the agreed
  sequence of its range up to the position it reports.
- Dannan's simulator runs the real code under injected faults. Kvasir
  supplies its sequential model and a rule per level saying which losses
  which faults excuse; the checker takes the isolation level, since a
  snapshot permits histories no serial order explains. The cluster step is
  not built until the commit protocol passes in it.
- Power pulled mid-write on a Pi closes the gap to the real thing.

## Compatibility and authentication (Proposed, on ruling 21)

- "MySQL compatible, not MySQL based." Nothing of MySQL, MariaDB or Aurora is
  copied. The engine is InnoDB to its clients; another engine is refused or
  substituted with a warning as MySQL does. The server reports
  `8.4.<n>-kvasir`.
- TLS before any authentication, through Dannan's `Stream::start_tls`;
  nothing sent before the upgrade is acted on. `caching_sha2_password` only,
  its full path over TLS with no RSA exchange; `mysql_native_password` and
  `mysql_clear_password` refused; an auth switch never downgrades. The
  fast-path verifier is in memory only and dropped on any change to the
  principal. `COM_CHANGE_USER` and `COM_RESET_CONNECTION` take the same path,
  back-off and audit. Names are compared as bytes. The user list is built in,
  from YAML, until the Lugh plugin. Mimir turned out to have no user list to
  shape it on, so: the `users` setting (secrets.yaml), each item
  `<name>:argon2id$m=<KiB>,t=<passes>,p=<lanes>$<salt hex>$<tag hex>`,
  Argon2id through Dannan's primitive (the stack's one home for password
  hashing, which Lugh uses); a name is checked with the same work whether
  or not it exists. Built as `kvasir/server/accounts.h`. The back-off per
  principal and per source, before any hashing, is still to build.
- The surface in the order built: connection and session statements; DDL;
  `INSERT`, `SELECT`, `UPDATE`, `DELETE`, `REPLACE`; types; expressions and
  functions; joins, grouping, ordering, subqueries, unions, common table
  expressions, window functions; transactions and locking reads; prepared
  statements; views; `information_schema` and `SHOW`; users and grants; then
  stored programs, triggers and events. Left until asked for: the
  replication protocol, XA, full-text and spatial indexes, loadable plugins.

## Sharding work across the cluster (Proposed, later)

Run the work where the data is and ship results, never rows. A scan is split
by range into pieces (filter, projection, partial aggregate, limit), each run
at a replica of its range, keyed by query and piece so a duplicate is
dropped, with a deadline, the first good answer taken and the rest cancelled.
Joins that would move rows between nodes are not attempted at first.

## Order (Proposed)

1. The skeleton. Done.
2. One node, in memory: the protocol, SQL and transactions for the subset
   real drivers and tools use, with the differential gate. In hand
   (`ACTIVE.md` says how far).
3. The commit protocol written down, Kvasir's model and level rules
   supplied, and the protocol passed through Dannan's simulator.
4. The cluster at `decided` and `replicated`: ranges, the range map in slot
   registers, sticky proposers, the transaction record, replicas in memory.
   Waits on Dannan 0.9.0.
5. Persistence: acceptor state in node-local Dagda logs, folding, the
   manifest head, backup and restore. Waits on Dagda's node-local log.
6. `ephemeral`, `persisted` and held commits; range splits and merges; then
   pieces of a query on other nodes, the Lugh plugin and a change feed.

## Waiting on the maintainer

Nothing blocks the work in hand. Proposed and not yet put to him:

- The default replica count (three).
- The proposals under "Acknowledgement levels" marked Proposed.
- The SQL surface left until asked for.
- Everything else in this document marked Proposed stands as the working
  design until he rules otherwise.

## What Kvasir asks of the others

| Of | What | Status |
|---|---|---|
| Dannan | Agreement, as designed: the fast path, deltas, the serving refusal naming the set, `transfer()` after catch-up | On its develop as 0.9; Kvasir's four asks accepted |
| Dannan | The slot table for the range map | Relayed as ruled |
| Dannan | The simulator, with the isolation-level checker, resolution events, faults in the history, correlated kills, membership in the world | In 0.9.0; first pieces merged |
| Dannan | The hybrid clock | 0.9.0 |
| Dannan and Mimir | An atomic commit across replica sets, written once | To be proposed when one of us reaches it |
| Dagda | A node-local log for acceptor state | Slice 4, paused |
| Dagda | The file contract its log writes through, so real Dagda runs in the simulator | With Dagda, asked by Dannan |
| Dagda | Objects, claims and one head space for manifests | Exists |
| Lugh | Principal events and the principal-status scope, as for Mimir | Designed for Mimir, not scheduled |
| Lir | The exported MQTT client library, as for Mimir | On Lir's develop |
