# Kvasir: design (draft, 2026-11-15)

Kvasir is a MySQL-compatible, multi-master, leaderless distributed SQL
database: a hosted system on Dannan, persisting in Dagda, written new from this
design. It is Mimir's sibling and takes Mimir's concepts; where Mimir is the
key-value store, Kvasir is the relational one, with the storage idea and the
resilience of AWS's Aurora.

Written by the Kvasir session from Tom's rulings in conversation and from
Mimir's design (`../mimir/TODO/2026-11-15-mimir-design.md`), which records what
the Dannan, Dagda, Lir and Lugh sessions have already agreed for Mimir. No
sibling session has reviewed this document yet. Each statement is marked:

- **Ruled**: Tom's decision, 2026-11-15.
- **Agreed (for Mimir)**: another session's answer to Mimir about its own
  project, which Kvasir expects to rely on and has not yet confirmed for itself.
- **Proposed**: the Kvasir session's recommendation. Nothing proposed is built
  until Tom rules and the owning session has reviewed it.

## Ruled (Tom, 2026-11-15)

1. **Kvasir is a sibling project to Mimir**, "using the same concepts and the
   standard style/deps (Dannan, Dagda, Lir, Lugh, etc)".
2. **Kvasir is a SQL database, MySQL client compatible.**
3. **It is "similar in concept and resilence to AWS's Aurora".**
4. **"The usual apply - leaderless, soft cluster, replicated, multimaster."**
   There is no writer node, no primary and no central coordinator; every node
   takes reads and writes.
5. **"It must offer ACID the same as other major databases."**

6. **"As an extension, Use Mimir's 3 level ack persistence per command or
   session (switchable)."** What the levels mean for a SQL transaction is
   under discussion with Tom and the Mimir session; see "Acknowledgement
   levels".

7. **The goal, in Tom's words:** "What we want is MySQL but fast, replicated,
   storage scalable like Aurora, and even with the possibility of sharding
   work across the cluster. In short, a soft cluster MySQL that's damn near
   impossible to properly take down with infra failures. The design will be a
   compromise, and that's where what is useful comes in." The draft is under
   review against this (`2026-11-15-design-review.md`), with the sibling
   sessions asked for advice.

8. **A sticky proposer is leaderless** (by interactive question, after every
   session's advice). Each range's commits go to whichever node committed on
   it last, which batches them into one decision; any node can take the range
   at any instant; no term, timer, election or failover procedure; safety by
   ballots, never clocks. Timed read leases were not chosen.
9. **A fourth acknowledgement mode is added: decided, disks trail.** The
   commit is agreed by a majority in memory before the node answers and
   synced after; the answer is never refused later. Levels 0, 1 and 2 stay
   as ruled. A per-table lowest level was offered and not chosen. Its name
   or number is not ruled.
10. **The second writer of a row waits, as InnoDB does** (error 1205 after
    `innodb_lock_wait_timeout`, 1213 on a deadlock), with the row locks held
    in memory at the range's proposer.
11. **The deterministic simulator is built once, in Dannan, proposed jointly
    with Mimir**; Kvasir's cluster step is gated on its commit protocol
    passing in it.

12. **The tagline is "Kvasir - The Distributed Relational Database".**
13. **The compatibility reference is MySQL 8.4 LTS**, and the differential
    gate runs against Oracle's MySQL 8.4 server on a build server.
14. **Kvasir commits when a step's gate passes and pushes `develop` to a
    private remote**, as Mimir does.
15. **The acknowledgement levels are named, not numbered, as a group
    standard**: `ephemeral`, `persisted`, `decided`, `replicated`, set by
    `ack_level` (`2026-11-15-ack-levels-standard.md`). Where this document
    says level 0, 1 or 2 it means `ephemeral`, `persisted` and `replicated`.

`2026-11-15-rationalised-design.md` is the current proposal and supersedes
the sections below wherever they differ (one truth through agreement, the
range as the unit, storage, authentication, the order). This document is to
be rewritten from it.

Rulings 4 and 5 together decide the shape: every transaction is atomic,
consistent, isolated and durable, and it commits without a leader. Mimir's
merge class (writes that keep working on both sides of a split and merge
after) cannot be ACID, so Kvasir has no merge class unless Tom asks for one
(see "Waiting on Tom").

## What "similar to Aurora" means here (Proposed)

Taken from Aurora, as concepts, nothing as code:

- **The log is the database.** A commit is durable when its log records are on
  a quorum of storage replicas. Nothing else is written in the commit path: no
  pages, no double write, no checkpoint by the database engine.
- **Storage materialises in the background.** The replicas fold the log into
  the stored form of the data out of the commit path, continuously, so a
  restart replays almost nothing and recovery time does not grow with uptime
  (discipline 4: history is compacted out of the hot path).
- **Storage is replicated by quorum across failure domains and heals itself**:
  a lost replica is rebuilt from the others without an operator, and a gap in
  one replica's log is filled from its peers.
- **A node's SQL state is soft.** Sessions, plans and cached data are rebuilt;
  losing a node loses no acknowledged commit, and a client reconnects to any
  other node.
- **Continuous backup, restore to a point in time, and cheap clones**, because
  the stored form is immutable and the log is kept.

Not taken: Aurora's single writer and its failover. Ruling 4 replaces them.
Aurora's own multi-master mode is the nearer shape: every node writes, the
storage quorum decides between conflicting writers, and the loser's
transaction is rolled back with a deadlock error its client already knows how
to retry.

Resilience, stated as Aurora states its own: with R replicas of a shard and a
majority quorum, Kvasir keeps accepting commits through the loss of any
minority of a shard's replicas and loses no acknowledged commit while one
majority's disks survive. R is 3 by default (one loss tolerated), as Mimir and
Dagda's `replicas(3)`, k = 2 log have it. Aurora's own figure is 6 copies in 3
zones with 4 to write and 3 to read; R and the failure-domain spread are
settings, so a cluster with three failure domains of two nodes can be given
that shape. See "Waiting on Tom".

## ACID (ruling 5; mechanisms Proposed)

- **Atomic.** A transaction's writes, on however many shards, take effect
  through one agreed decision (its transaction record, below) or not at all. A
  node that dies mid-transaction leaves nothing another node cannot resolve on
  its own (law 4, discipline 3).
- **Consistent.** `NOT NULL`, `CHECK`, primary key, unique and foreign key
  constraints are decided inside the agreed commit, never by a node's local
  view. A unique index is a keyed structure of its own, so two inserts of one
  value on two nodes contend for one register and exactly one commits.
- **Isolated.** Multi-version rows. MySQL's four levels are accepted and mean
  what InnoDB's mean or stronger: `READ UNCOMMITTED` is served as `READ
  COMMITTED`; `REPEATABLE READ` (the default, as MySQL) reads one snapshot for
  the transaction; `SERIALIZABLE` is serializable. Writers of one row never
  both commit unseen by each other.
- **Durable.** A commit is acknowledged to the client only once a majority of
  each written shard's replicas have it on disk. That is level 2 of ruling 6;
  levels 0 and 1 are a session's stated choice to be answered sooner (see
  "Acknowledgement levels"). Until persistence is built (step 5) Kvasir cannot keep this, and says so
  (see "Before persistence").

## Acknowledgement levels (ruling 6)

**Ruled (Tom, 2026-11-15), in his words:** "0 - node returns immediately and
the commit is actioned async. 1 - node returns once local persistence acks.
2 - node returns once majority all replication persistence acks. The intent
is 0 - ephemeral/disposable data, 1 - persisted, 2 - replicated."

So the level is where the node returns to the client, and at level 0 that is
before the commit has been actioned at all, not after it is agreed. The
session's earlier reading below had level 0 return after agreement in a
majority's memory; that is superseded for level 0, and the rest of it stands
only where it does not contradict the ruling. What follows from the ruling
and is still to be settled with Tom: whether a level 1 commit is decided by
the cluster before the node returns, and what becomes of a commit that was
answered early and then refused (a conflict, a constraint, no majority).

**Ruled further (Tom, 2026-11-15, by interactive question):**

- **Level 1 is local disk only; the cluster's decision is async**, as Mimir's
  level 1. So at levels 0 and 1 the node's answer is given before the cluster
  has decided the commit, and an answered commit can still be refused.
- **At level 0 the node makes its local checks before it answers** (syntax,
  types, `NOT NULL`, `CHECK`, and what it can see locally of keys and
  conflicts), so a plainly bad statement still gets its MySQL error; only the
  cluster's decision is async.
- **The default level is set in YAML and has no built-in value**: a node
  without it stated refuses to start.

- **An answered, undecided commit is seen only by its own session.** Every
  other session, on any node, sees it once the cluster has decided it. A
  level 2 transaction therefore never builds on data that is later refused,
  and level 2 stays ACID beside the other levels.
- **A commit refused after it was answered is rolled back whole, counted on
  Status, recorded in a system table the application can query, and the
  session's next statement carries a warning.** Later commits of that session
  that built on it are refused with it.
- **On a minority side, a commit at level 0 or 1 is accepted, held and
  decided at heal** (the soft cluster): held on the node's disk at level 1
  and in memory at level 0, up to a stated bound, and put to the cluster when
  a majority is reachable. What the majority agreed meanwhile wins. Level 2
  is refused on a minority, with an error.
- **Level 2 always wins**: a level 2 transaction contending for a row with
  another node's answered, undecided level 0 or 1 commit overrides it, even
  if that one was put to the cluster first. Once a commit is decided it
  stands, whatever its level.

What the rulings make of ACID. At level 2 a transaction is ACID as ruling 5
says. At levels 0 and 1 the node's answer means accepted, not committed: what
is given up is durability and the certainty of the answer, by the session's
own choice. The database itself never holds a state that breaks a constraint
or shows part of a transaction, at any level, because nothing is visible to
others until the cluster has decided it and a refusal is whole.

Proposed, following from the rulings and not yet put to Tom: between levels 0
and 1 neither has priority, the first decided stands; a session's held
commits are put to the cluster in the order it made them; a node at its bound
of held commits refuses further ones at levels 0 and 1 with an error rather
than dropping the oldest; a level 1 node that restarts puts its held commits
to the cluster again from its disk (recover by resolving); the system table
is `kvasir.refused_commits`, bounded, naming the session, the time, the
statements' digest and the reason.

What MySQL does, for that discussion. MySQL never answers a commit and then
refuses it for a conflict: on one server the decision is made before the
answer, and in its multi-primary forms (Group Replication, Galera) `COMMIT`
waits for certification and fails itself (error 3101, or 1213). What MySQL
does have is commits answered and then lost: with
`innodb_flush_log_at_trx_commit` 0 or 2 a crash loses about the last second
of answered commits, whole and silently; with asynchronous replication a
failover loses what the source had answered and not yet sent, silently. The
one MySQL product that accepts writes on two sides and settles them later,
NDB Cluster's replication conflict resolution, rolls the losing transactions
back whole and records each in an exceptions table the application can read.

### The session's earlier reading (superseded in part)

Mimir's ruling 7: level 0, written to memory, acknowledged at once; level 1,
written to local disk, then acknowledged; level 2, replicated, acknowledged
once a majority of the key's replicas have it. Kvasir's reading for a
transaction:

- The level says only **when `COMMIT` is acknowledged to the client**, that
  is, how much of D the client waits for. It never changes how the commit is
  decided: every commit is still one agreed decision on a majority, so A, C
  and I hold at every level. Kvasir has no merge class, so there is no write
  that skips agreement at a lower level, as a Mimir merge write does.
- **Level 2** is ruling 5's durability and is the default: acknowledged when a
  majority of each written shard's replicas have the commit on disk.
- **Level 1**: acknowledged when the commit is agreed and is on the
  coordinating node's own disk; the other replicas sync after. An
  acknowledged commit is lost only if that node's disk is lost and a majority
  of the replicas lose power before they sync.
- **Level 0**: acknowledged when the commit is agreed, held in a majority's
  memory, with every disk write after. An acknowledged commit is lost only if
  a majority of the shard's replicas stop before they sync.
- At every level a commit that was acknowledged and then lost is lost whole,
  never in part, and no later transaction that saw it survives it: the
  replicas agree on what was lost. To be shown, not assumed.
- **Switching**: a session variable (`kvasir_ack_level`, with `SET SESSION`
  and `SET GLOBAL` as any MySQL variable), a per-statement form as an
  optimizer hint (`COMMIT /*+ SET_VAR(kvasir_ack_level=0) */`, MySQL's own
  syntax for a variable held for one statement), and the YAML default. The
  level in force at `COMMIT` covers the whole transaction.
- **Mimir's answer (2026-11-15).** The three lines are right. Level 2 is a
  majority on disk (Tom to Mimir: "Majority is enough"). For an agreement
  command the level says what each acceptor does before it answers: memory at
  level 0, its own sync at levels 1 and 2, so Mimir sees no difference between
  1 and 2 there and proposes that agreement commands treat 1 as 2 (not ruled).
  Every Kvasir commit is an agreement, so the level 1 above, "the
  coordinator's disk, the others after", is a different meaning from Mimir's
  and Tom chooses between them. Level 0 stays a loss of D only because of
  Dannan's restart rule (its agreement design, "Membership and restarts"): a
  replica that lost its state does not vote until it has caught up. Mimir's
  ruling 16 concerns merge writes only. Tom has not ruled a default level for
  Mimir, nor the spelling of the switch; Mimir asks that one spelling be ruled
  for both projects.
- Other databases offer the same relaxation and still call themselves ACID:
  `innodb_flush_log_at_trx_commit` 0 and 2 in MySQL, `synchronous_commit` in
  PostgreSQL. Levels 0 and 1 are that, said per session.

## Shape (Proposed, from Mimir's)

- **One process per node**, `kvasir-node`, advertising capability `kvasir`.
  Pins Dannan 0.8.2 today; 0.9.0 changes the ABI, one rebuild.
- **Clients** speak the MySQL client/server protocol (protocol 10, the classic
  protocol; not the X protocol) to any node, over `dannan::listen`'s
  `StreamListener`. The packet reader, the SQL lexer and the parser are
  parsers of untrusted input, each with a fuzz target. A per-client outbound
  byte bound closes a slow client. MySQL upgrades a connection to TLS in
  place, after a plaintext greeting; that needs a stream that can do so, which
  is asked of Dannan below.
- **Shards.** A table's rows are ordered by primary key and split into shards
  by key range, since SQL scans ranges; a hash of the key, as Mimir uses,
  would make every range scan a scan of every shard. Each secondary index is a
  keyed structure of its own, split the same way. A shard has R replicas chosen
  by `placement::by_capacity` with Kvasir's salts among nodes advertising
  `kvasir`, spread over failure domains. A node outside a shard's set proposes
  to and reads from its replicas with `Routes::call`; the client never sees
  which node holds what. The shard map (which ranges exist) is itself agreed
  state, and a split or merge is an agreed change to it. To be designed in
  full before step 3.
- **Work classes** (Agreed for Mimir, Dannan). A client's statement is
  foreground and runs on the connection's strand; replication a commit waits
  on is foreground; a replica catching up is loader; materialising the log,
  collecting old row versions, statistics and anti-entropy are speculative.
  The worker pool is for work that blocks or is long: sorts, joins and scans
  past a bound.
- **Memory.** Unlike Mimir, the data does not fit in memory. Each node keeps a
  bounded cache of materialised data and the unmaterialised tail of its
  shards' logs, declared and reserved in the node's `Budget`. A statement that
  cannot reserve what it needs is refused with MySQL's out-of-memory error; it
  never grows a buffer without a bound.

## Transactions without a leader (Proposed; to be designed with the Dannan session)

The mechanism is Dannan's agreement primitive (Mimir's ruling 6, built in
Dannan 0.9.0): a linearizable change to named registers over a replica set the
caller supplies, proposed by any node, committed on a majority, several
registers of one replica set in one round, with evidence. Kvasir is its
fourth consumer after Mimir, Lugh and Louhi.

- **The coordinator** is the node the client is connected to. It holds the
  transaction's state in memory; nothing about it is special, and any node
  can finish or abort its work.
- **Time.** Snapshots and commits are ordered by Dannan's hybrid logical clock
  (Mimir's ruling 8; Kvasir waits for it and writes no copy). A transaction
  reads at its start timestamp and commits at a later one.
- **Writes are intents.** A statement that writes a row lays an intent on it
  by agreement at the row's shard: the new version, marked with its
  transaction. A second writer finds the intent and waits for that
  transaction to end, up to `innodb_lock_wait_timeout`, then gets error 1205,
  as InnoDB's row lock behaves; a cycle of waiters is broken with error 1213.
  `SELECT ... FOR UPDATE` and `FOR SHARE` lay lock intents the same way. An
  autocommit statement on one shard lays its intents and commits in one round.
- **The transaction record** is one register, on the shard of the
  transaction's first write: pending, committed at a timestamp, or aborted.
  Commit is one agreed change to it, made after the constraint checks, and is
  the single decision that makes every intent of the transaction, on every
  shard, a committed version (atomicity across shards without a coordinator
  that must survive). Intents are resolved into plain versions afterwards, out
  of the client's wait.
- **A reader that meets an intent** consults its transaction record: committed
  before the reader's snapshot, it is read; aborted, it is skipped; pending,
  the reader waits or, when the writer's node has left the roster, aborts the
  record itself by agreement (recover by resolving). A transaction record whose
  coordinator's incarnation is gone is aborted by whoever meets it; one whose
  coordinator is alive is extended by that node's heartbeat and never aborted
  under it.
- **Write conflicts.** A transaction that goes to write a row committed by
  another after its own snapshot is refused with error 1213 at `REPEATABLE
  READ` and above (first committer wins), and proceeds at `READ COMMITTED`,
  as InnoDB's semi-consistent write does.
- **Reads inside a transaction must not miss a commit below their snapshot.**
  With no leader, a single replica cannot know that no commit with an earlier
  timestamp is still being decided. The proposal: a replica that serves a read
  at timestamp T promises to accept no commit at or below T on that range, and
  a read inside a read-write transaction is confirmed with a majority (the
  agreement primitive's strict read, an identity proposal). This is the
  hardest open point in the design and is to be settled with the Dannan
  session before step 3.
- **Minority side (the soft cluster).** A node that cannot reach a majority of
  a shard's replicas refuses writes to it within the statement's deadline,
  with an error that says why. It still serves reads from what it holds,
  possibly stale, to sessions that have asked for that; by default a read is
  strict and is refused too, since a stale read inside a transaction is not
  isolation. A session opts in with a variable (`kvasir_stale_reads`), for
  autocommit `SELECT` only.
- **`AUTO_INCREMENT`.** Each node reserves a block of values for a table by
  one agreed change and hands them out locally: unique, increasing on one
  node, with gaps and not ordered between nodes, which MySQL does not promise
  either.
- **DDL.** The catalog (schemas, tables, columns, indexes, users, grants) is
  agreed state with a version. A statement runs against one catalog version,
  and a commit is refused if the tables it wrote have changed shape since. DDL
  commits implicitly, as in MySQL. Building an index on a populated table is
  loader work with a visible state, not a held lock.

## Storage: the log is the database (Proposed; waits on Dagda's contract v2)

Stack rule 2: what Kvasir persists lives in Dagda. In Dagda's terms, following
what Dagda agreed for Mimir:

- **One log per shard**, a `replicas(R)` log opened with a majority k, group
  committed. An intent, a transaction record's change and an intent's
  resolution are log entries. A commit is durable when its entries are
  appended at Dagda's replicated level. The agreement primitive's acceptor
  state (promises and accepted values) goes to a node-local log on each
  acceptor, which has no rollback, as Dagda recommended to Mimir.
- **The stored form is immutable.** Each replica folds its log, in the
  speculative class, into sorted, immutable chunks of row versions, stored as
  Dagda objects with claims: a log-structured merge of chunks rather than
  pages updated in place, because Dagda's objects are immutable and
  content-addressed. One small head per shard names its chunks and the log
  position they cover, written atomically, so a restart opens the head and
  applies only the log's tail.
- **Reads** take a row from the cache, else from the chunks the head names,
  else from the log's tail. The cache is bounded and ephemeral.
- **Old versions** are dropped during folding once they are older than every
  open snapshot and than the restore window.
- **Restore to a point in time** is a new head naming the chunks and log
  prefix as of that timestamp, kept available for a configured window by
  claims. **A clone** is a new set of heads claiming the same objects; it
  shares storage until either side writes.
- **Healing.** A replica behind the log's truncation recovers by taking the
  head and its chunks from a peer, never by partial replay; Dagda's repair
  restores lost copies of objects; a scrub re-reads and checks them.
- Kvasir writes to Dagda as its node principal.
- Unknown until Dagda's slice 4: the log's inline limit and its commit rate.
  The group commit sets Kvasir's transaction rate, so it is measured before
  anything is promised.

### Before persistence

Steps 2 to 4 run in memory. In that time a commit is acknowledged once a
majority of replicas hold it in memory, which is not durable. Proposed: the
server says so in its version comment, in a warning on every connection's
first statement and on Status, and the README's status says it is not yet
ACID. Mimir refuses its durable levels until Dagda's log exists (its ruling
17); refusing every commit would leave Kvasir with nothing to test, so the
proposal is to run and say so.

## Compatibility (Proposed)

- **"MySQL compatible, not MySQL based."** The specification is the MySQL 8.4
  Reference Manual and the documented client/server protocol. Nothing of
  MySQL, MariaDB or Aurora is copied; MySQL's GPLv2 would not allow it under
  GPLv3 in any case.
- **The gates.** Tests written from the manual, and a differential gate: a
  GoogleTest case set, run by hand on a build server, that sends the same
  statements to `kvasir-node` and to a MySQL 8.4 server and compares result
  sets, column metadata, warnings and error numbers. Byte equality of whole
  packets is not the bar, as it is for Mimir's RESP: connection ids, timings
  and plan-dependent row order differ legitimately.
- **The engine is InnoDB to its clients.** `ENGINE=InnoDB` is accepted and is
  the default; another engine is refused, or substituted with a warning as
  MySQL does without `NO_ENGINE_SUBSTITUTION`.
- **The server reports** `8.4.<n>-kvasir` as its version, so connectors choose
  8.4's behaviour, and `kvasir_version` as a variable.
- **Authentication** is `caching_sha2_password`, MySQL 8.4's default: its fast
  path against a verifier held in memory, its full path only over TLS (no RSA
  key exchange). `mysql_native_password` is off by default, as in 8.4, with a
  setting to turn it on. The user list is built in, from YAML, until the Lugh
  plugin (as Mimir's: secrets bound to a Lugh principal, revocation over Lir,
  never failing open).
- **The surface, in the order it is built**: connection and session
  statements; DDL for schemas, tables and indexes; `INSERT`, `SELECT`,
  `UPDATE`, `DELETE`, `REPLACE`; the types; expressions and the function
  library; joins, grouping, ordering, subqueries, unions, common table
  expressions and window functions; transactions and locking reads; prepared
  statements over both protocols; views; `information_schema` and the `SHOW`
  statements connectors and tools issue; users and grants; then stored
  programs, triggers and events. Left until asked for: the replication
  protocol (binary log dump), XA, full-text and spatial indexes, loadable
  plugins and functions, and engines other than InnoDB.

## Order (Proposed)

1. The project skeleton, by the style document's checklist.
2. One node, in memory: the protocol, the lexer and parser, the catalog, the
   types, the executor and multi-version transactions, tested from the manual
   with the differential gate.
3. The cluster by agreement: shards, intents, the transaction record,
   constraints, `AUTO_INCREMENT`, agreed DDL. Waits on Dannan 0.9.0's
   agreement primitive and hybrid clock.
4. Range splits and merges, placement changes, and a replica catching up.
5. Persistence: the log in Dagda, folding, heads, the bounded cache, and
   durability as ruling 5 requires. Waits on Dagda's contract v2.
6. Restore to a point in time and clones; then the Lugh plugin.

## What Kvasir would ask of the others

Nothing has been asked yet; these go to the sibling sessions once Tom has
ruled on this design.

| Of | What | Status |
|---|---|---|
| Dannan | The agreement primitive, as Mimir's ruling 6, with a majority-confirmed read that fences a timestamp | In Dannan 0.9.0 for Mimir; Kvasir's needs not yet put |
| Dannan | The hybrid logical clock | In Dannan 0.9.0 for Mimir |
| Dannan | A listener stream that upgrades to TLS in place after a plaintext preface | Not asked |
| Dagda | The log and contract v2, a node-local log for acceptor state | Slice 4, paused; asked by Mimir |
| Dagda | Objects, claims and one small head per shard, at the rate folding writes them | Exists; the rate is unmeasured |
| Lugh | Principal events and the narrow principal-status scope, as for Mimir | Designed for Mimir, not scheduled |
| Lir | The exported MQTT client library, as for Mimir | On Lir's develop |
| Mimir | Nothing shared yet. What both write twice (a command or statement router over shards, intent and waiter records dropped by incarnation) is proposed to Dannan under rule 3 when the second copy would be written | Not asked |
