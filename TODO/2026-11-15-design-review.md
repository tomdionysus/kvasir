# Kvasir: review of the draft design (2026-11-15)

The Kvasir session's own check of `2026-11-15-kvasir-design.md`, made at the maintainer's
request ("Does this make sense for a distributed database? What is useful,
what isn't, what's impossible from a CompSci point of view?"). From first
principles and from memory of the published systems; nothing here has been
modelled or tested. The sibling sessions were asked for their experience and
opinion, advisory only; their answers follow.

This is a record and is not revised: it reviews the first draft, which the
second draft (`2026-11-15-kvasir-design.md`) has replaced, and some of what
it says was settled afterwards by the maintainer's rulings there. Keep it for the
sessions' measurements and warnings, which the design cites only in brief.

## The goal (the maintainer, 2026-11-15)

"What we want is MySQL but fast, replicated, storage scalable like Aurora, and
even with the possibility of sharding work across the cluster. In short, a
soft cluster MySQL that's damn near impossible to properly take down with
infra failures. The design will be a compromise, and that's where what is
useful comes in."

## Sound

- ACID at level 2, refused on a minority side. CAP forbids ACID writes on
  both sides of a split, and the design refuses them there.
- One transaction record deciding intents on many shards: a known, correct
  atomic commit with no coordinator that must survive.
- Levels 0 and 1 as "accepted, decided later": the tentative-write model. It
  is sound because nobody else sees the commit until it is decided.
- Immutable chunks and a log in Dagda: a log-structured store suits an
  immutable object store.

## Impossible, or possible only in a weaker form

- **"Level 2 always wins."** A proposer cannot tell a commit in flight from
  one already chosen by a majority. Level 2 can pre-empt an undecided commit
  only until that commit is chosen: a priority, not an absolute.
- **Recording every lost commit.** A level 0 commit held in memory on a node
  that dies is lost with nothing durable to record it from. Only refusals can
  be recorded.
- **Read-your-writes at levels 0 and 1 across connections.** "Its own session
  only" fails under a connection pool.
- **Guaranteed progress under contention.** Leaderless consensus can duel
  without end (FLP); backoff makes progress probable, never certain. Hot rows
  and index tails contend.
- **Cheap consistent reads with no leader.** With no lease holder and no bound
  on clock skew, a read that must not miss an earlier commit asks a majority.
  Aurora avoids that only by having one writer.
- **Strict ordering across shards.** Hybrid clocks give serializable; a
  transaction may still miss a commit that finished just before it began on
  another node. Closing that needs a clock bound or further rounds.

## Defects in the draft

- **Two replication mechanisms for one job.** Commits are agreed through
  Dannan's primitive, whose acceptors must persist, and also appended to a
  replicated Dagda log: each commit replicated twice, and a multi-writer log
  gives no total order. One of them must be the truth.
- **Phantoms.** Agreement per row cannot stop an insert into a range another
  transaction scanned. `SERIALIZABLE` needs a register per range, which
  worsens contention. Snapshot isolation for `REPEATABLE READ` avoids it.
- **Intents by agreement on every writing statement**: a consensus round per
  statement, and distributed deadlock detection. Checking at commit
  (optimistic), as Aurora DSQL does, is one round per shard; the session now
  recommends that, reversing its earlier advice.
- **Level 2 after held commits.** A session's level 2 commit must first wait
  for its own held commits to be decided, or it builds on undecided data.

## Useful and not

- Level 2 is the database. Level 0 suits telemetry, caches and session data.
- Level 1, and holding commits through a split, suit insert-only data with
  keys the node can mint. Updates to shared rows made against stale data are
  mostly refused at heal. `AUTO_INCREMENT` stops when the node's reserved
  block runs out.
- A gap: no level gives "the answer is certain, only durability trails", the
  relaxation PostgreSQL and MySQL users use most.

## The decision that matters most

Spanner, CockroachDB, TiDB and YugabyteDB look leaderless and multi-master
from outside and use a short-lived, automatically chosen lease holder per
range inside. If "leaderless" means no designated node and no failover
procedure, per-range leases satisfy it and make reads and commits far cheaper.
If it means no leases at all, the design works and pays a majority round on
every read and risks duelling writers.

Apart from the cluster: MySQL compatibility (collations, the optimiser, the
function library, `information_schema`) is the largest piece of work, and
needs a defined subset as its gate.

Before the cluster step is built: the commit protocol written down and tested
by deterministic simulation with injected faults and a history checker.

## What the sibling sessions said

Advisory. Each is the session's own account, condensed; none had work asked
of it. Asked 2026-11-15: Dannan, Dagda, Mimir, Lir, Lugh, Louhi, Macha Server,
the coordinating session, Miach, Corvus Radioberry, Corvus LoRa, Hero Graphics.

### Corvus Radioberry (measured on Pi 4 and Pi 5; had not read the drafts)

- **Brownouts are load-correlated.** A cluster Pi 5 dropped off the network
  under two concurrent heavy jobs. Loss of a node comes when the cluster is
  busiest, and repair and rebuild traffic loads the survivors: recovery work
  needs a throttle and a budget so it cannot cascade into the next brownout.
- **Slow is a failure mode.** A Pi 4 under-voltage sat at 600 MHz: up,
  answering heartbeats, two to three times slower. Health by latency, not
  only liveness; read `vcgencmd get_throttled` and expose it. Peaks of
  current matter, not the average.
- **Periodic network stalls.** A Pi on Wi-Fi rescanned every 33.7 s for
  3.65 s, stalling UDP about 130 ms each time whatever the power-save
  setting. Tight failure detectors trip on it and anything leased flaps:
  timeouts well above it, or wired links for voting members. Drops under
  load were at the receiver (`UdpRcvbufErrors`); check both ends' counters.
- **Clocks.** A Pi has no RTC by default and boots with a stale clock until
  NTP syncs. Order nothing by wall clock, and refuse a lease or any
  time-dependent role until time is known to be synced.
- **Fail-safe.** The safe state is what holds with no software running;
  permission is renewed by a watchdog, never latched. Verify the effect of a
  limit, not its configuration. Never truncate or reset a replica's log while
  a stream may still be applying it (their worst lock-up was a clear racing
  live data): reconcile by sequence number, drain, never clear. Every link
  carries a CRC and sequence numbers; gaps are counted in sticky counters so
  an intermittent fault leaves evidence. Idle costs nothing.
- **As a user of a database**: honest, documented consistency per operation
  with no silent downgrade under partition; writes acknowledged only when
  durable on the stated number of nodes; per-node health that includes
  "slow"; backpressure, not unbounded queues; an upgrade path tested from the
  previous release.

### Dannan (its designs: `../dannan/TODO/2026-11-15-agreement-design.md`, `2026-11-15-hlc-design.md`)

- **Agreement cost.** CASPaxos. Two round trips per decision, one on the
  fast path when the proposer committed that register last and nobody else
  touched it. Each acceptor syncs once per phase before answering (two
  syncs, or one fast); on Dagda's node-local log they are group-committed in
  a 10 to 20 ms tick. Concurrent proposers on one register duel: the loser
  retries with backoff, progress probable and not certain. "A hot row is the
  case to design around."
- **Granularity is Kvasir's.** Several registers of one replica set commit
  atomically in one round; not across sets, so a multi-shard commit needs the
  transaction-record scheme. A register can be large (Mimir's are whole keys,
  megabytes; prepare carries ballots and a version, accept a delta). Per-row
  registers mean many agreements per transaction; per-range registers mean
  contention.
- **Reads.** Without a lease or a skew bound a strict read is a majority
  round. The hybrid clock refuses stamps more than `max_ahead` ahead (a
  minute by default): a loose bound, not a commit-wait bound, and Dannan does
  not measure skew on the wire. A per-range lease chosen through agreement
  is buildable on 0.9.0, but its safety rests on bounded clock drift while
  held, which the stack does not measure.
- **Is a lease holder "leaderless"?** the maintainer's to rule. So far he has accepted
  soft leases (Dagda's) and linearizable registers, and ruled Lugh's TOTP a
  register rather than a lease.
- **What fails on the Pi cluster**: the Wi-Fi link by far (0.5 MB/s and a few
  hundred calls a second between Pis, spikes of 8 to 37 ms and more); slow
  nodes under load; restarts and processes killed mid-write; NAT and
  reachability changes; clock trouble mostly unseen, and unmeasured. Design
  first for slow, lossy links and slow nodes, not crisp crashes: a deadline
  on every call, backpressure by work class, refusing at the edge, recovery
  that never needs every node; then durability across process kills, no
  rollback of node state, and deterministic fault simulation.
- **Sharding query work**: the pieces exist (`Routes::call`, work classes
  and `ClassGate`, `Budget`, `placement::by_capacity`) and are not assembled
  for it. Move computation to the data and ship results, never rows; bound
  every fan-out by deadline and memory and cancel stragglers; the scan side
  is loader class, not foreground; batch, since 0.8 copies a node's record on
  every call (fixed in 0.9.0).
- **On the draft**: the "two replication mechanisms" defect is right. If
  commits go through agreement, the acceptors' durable state is the truth and
  a Dagda log is the acceptors' store, not a second replication. Not in
  Dannan: leases with a skew bound, and a deterministic simulator, which
  would be worth building once for the stack (Mimir needs one too).

### Louhi (its design: `../louhi/TODO/2026-11-15-louhi-design.md`)

- A correction: Louhi has no single active scheduler and does not use an
  agreement primitive; the maintainer chose placement functions now and a lease-based
  scheduler revisited later. (Louhi corrected itself later the same day:
  Dannan does have an agreement primitive in design, and its "Leases: two
  guarantees, named" section separates Dagda's soft leases from a
  linearizable lease kept as a register.)
- **There is no "exactly one" on this stack, only "at most briefly two".**
  Dagda's lease is a soft claim, two holders can coexist for an absence
  window, and today is seen on one node only. Safety comes from the fence:
  every side effect carries its epoch and the receiver refuses a lower one.
  For a database the commit record or log append must itself check the
  epoch. Nothing here gives exactly-once; design for at-most-briefly-twice
  with fences and idempotency and say so.
- **Leases and "leaderless"**: the maintainer's to rule. Louhi's reading is that he
  objects to a fixed or hand-failed-over leader, not to a short automatic
  lease.
- **Pieces of a query on other nodes.** Rosters differ briefly between
  nodes, so two coordinators can place one piece differently: key each piece
  (query id, piece id) and make it idempotent. Slow cannot be told from
  dead: a deadline on every piece, re-place on the deadline or a roster
  change, take the first good answer and cancel the rest. Declare the unit
  of work and refuse or spill rather than grow (discipline 5). Capacity
  adverts are stale: refuse with backpressure at the receiver. Wi-Fi limits
  long before CPU: push filters and aggregates down.
- The Pi kernel on the build Pis boots with `cgroup_disable=memory`: a
  memory limit set that way is silently discarded.
- **Wanted from Kvasir first**: level 2, correct, with the wire subset
  common drivers and ORMs connect to, before any sharded query work; clear,
  retry-safe refusals; authentication through the stack; per-database
  quotas from the start; backup and point-in-time restore before replication
  features ("replication copies a bad DELETE perfectly").
- **Warnings**: read the sibling code, not only summaries; every trusted
  member can write every shared space; pin exact refs and check they are
  pushed; a CMake cache entry declared `PATH` mangles a URL passed by CI,
  declare it `STRING` (Lir and Louhi both had every hosted job fail on it).

### Corvus LoRa (from what it has hit; had not read the drafts)

- **Counters that must never repeat are the hardest records.** The danger is
  not losing an allocator but two copies issuing after a restore or failover.
  Fence every allocator, sequence and auto-increment owner, make takeover
  explicit, and never let a restore from backup resume issuing without proof
  the old writer is dead.
- **Two classes of data, kept apart**: a ledger and keys that cannot be lost,
  and telemetry rebuilt in seconds. A database that lets a table declare
  "losable, fast" against "durable, quorum" fits; one level for everything is
  too slow for one or too weak for the other.
- **Reconnection is about pacing.** What was held while a node was away must
  drain at a bounded rate, important first, or the reconnect becomes the
  outage. A replica catching up is throttled and never starves live traffic.
- **Failures are correlated power, not single disks**, and coincide with
  peak load. Place replicas across power domains.
- **3 GB machines are in the cluster**: a design needing every node to hold a
  full replica or large memory excludes them.

### Dagda (measured, and from Macha's port; read the review page only)

- **Chunks as objects and a head per shard is Dagda's natural shape**, and
  Macha's own: immutable 4 MiB extents as content-addressed objects with
  claims and a small mutable head naming them. Large immutable objects run at
  the disk's speed.
- **Where it fights**: a head commit is expensive, about 85 to 95 a second
  per node group-committed on a Pi (Macha's live load is about 1 a second):
  one per compaction or manifest change, never one per SQL commit. Only
  every-node head spaces exist today (`replicas(N)` is slice 2), so every
  node holds every shard's head. The log is designed, not built (slice 4,
  paused).
- **The agreement must be the truth; Dagda's log cannot be the whole of
  it.** The log merges by dot and orders only within an origin; "one
  appender at a time" would need a lease, and Dagda's leases are soft. Take
  the commit order from Dannan's agreement and make each acceptor's durable
  state a node-local Dagda log (Dannan's design names it the persistent
  `AcceptorStore`). Each commit is then replicated once. Chunks are built
  later from decided entries.
- **Rates, Pi 5 with NVMe**: large objects about 115 MB/s per node, 220 MB/s
  over ten concurrent. Small objects: 64 KiB files made durable through a
  500 ms group commit cost about 3.8 ms of CPU each across two nodes. Heads:
  85 to 95 commits a second per node, and a follower fell 10 to 22 s behind
  and the gap grew. The log prototype (not the product): p99 12.5 ms at a
  10 ms tick on a Pi. It falls over at thousands of separate small durable
  objects a second per node, and at any per-commit head commit. A few-MiB
  chunk rewritten by compaction is comfortable. No measured ceiling on object
  count.
- **Aurora-like storage, already there**: capacity-weighted placement across
  failure domains; prompt second copy, then repair as the guarantee; several
  disks per node, a lost disk drained or repaired; scrub; rebalancing on
  join; durability barriers with evidence; pacing. **Not there**:
  `replicas(N)` heads, erasure coding, tiering; shard splitting is Kvasir's.
  Repair ran about 3.6 to 6 MB/s on the live pair, slow by design.
- **Restore points and clones by claims are sound**: claim a clone's chunks
  before the source can release them, and keep every chunk a restore point
  names claimed by it.
- **What broke stored data in the port**: compaction losing removal and
  touch records under many small writes; an fsync failure must latch the
  domain and heal from disk; an in-place migration that must not need twice
  the storage; a harness not in production configuration gave wrong figures.
- **Insists**: never durable without the evidence; never delete what a claim
  might need; never restore node-local state from an older copy; a deadline
  on every wait; every byte through Dagda, sealed, no side files; the commit
  protocol proven by deterministic simulation.

### Lir (its cluster runs in memory; it does not yet persist in Dagda)

- **Which side of a split am I on** must be decided early, per node
  (`Nodes::watch_partition`; an even split is a minority).
- **Dots across restarts** must never repeat: Lir reserves sequence blocks in
  a durable node-local space. Use the hybrid clock from day one.
- **A moved connection loses its session state.** MySQL sessions hold more
  than MQTT's (transactions, temporary tables, prepared statements): decide
  early not to move them. MySQL has no redirect: plan for DNS or a load
  balancer in front, and clients that reconnect.
- **Tests** flaked until they waited on a convergence read, not on time:
  expose such reads from the start.
- **Dagda's rate** (corrected by Lir later the same day): Dagda's slice 0
  prototype did measure its log on a Pi: three processes, k = 2, 8,000
  appends a second from outside the replica set, p99 12.5 ms at a 10 ms
  group-commit tick and 23.5 ms at 20 ms, the same with 256 logs on one
  shared segment (a segment per log failed). That is a prototype, not the
  released log, and nothing has run end to end through a hosted system. A node advertises at most 64 things, so a space per shard
  is refused at about the 30th pending Dagda's advert aggregate: count spaces
  per node.
- **Listener**: bound everything per client (Lir closes a slow reader at
  4 MiB queued); a client certificate that did not verify is not "no
  certificate"; let close drain what was queued. Dannan's
  `Stream::start_tls` is server-side StartTLS, built for Lugh's LDAP, and is
  what MySQL's in-place upgrade needs.
- **Held work**: in memory means lost, say so of level 0. A held commit
  re-proposed after a heal can be decided twice unless decided and refused
  commits are remembered for a stated window. One owner finishes a dead
  node's held work (the origin, or a lease holder once the origin has left
  the roster). Bound the hold and say what happens at the bound (Lir sheds at
  80% of its budget and refuses at 90%). A restarted node checks the decided
  record before re-proposing.
- **Advice**: one source of truth per fact (the maintainer ruled one noun per kind of
  state for Lir); leases only where one writer matters, and Lir "would take
  short automatic leases if the maintainer's leaderless allows it"; give way under
  pressure in a stated order (shed, refuse writes, refuse connections), never
  crash.

### the coordinating session (operating the Pi cluster)

- **Clocks, measured on the four build Pis**: systemd-timesyncd, each to a
  different pool server, every 34 minutes. Offsets within a few ms, jitter
  0.6 to 12.3 ms, oscillators up to 12.5 ppm (about 25 ms of free drift per
  poll), and no error bound reported. Realistic spread 5 to 15 ms, sometimes
  more, unguaranteed. No correctness may rest on wall clocks; anything that
  needs wall time carries an explicit uncertainty and refuses beyond it.
  chrony against one LAN source (a GPS one exists upstream) would do better.
- **Power is the failure that happens**: 110 under-voltage events on one Pi
  in 24 hours. Writes interrupted by brownouts, CPUs throttled
  mid-transaction, nodes slow rather than down because they are also the
  build machines.
- **A real partial partition**: one node's ssh timed out while its HTTP
  answered; its scheduler lost the acting scheduler's heartbeat for 90 s and
  began acting too. Harmless only because every action was idempotent on one
  key. Assume two leaders at times; fence by term or epoch, never by "I
  think the other is dead". Detect half-dead nodes by progress.
- **Upgrades**: mixed versions live side by side for hours. Versioned
  formats, read old, refuse unknown; one node at a time. "Cannot read" means
  "possibly in use", never free.
- **Asks**: odd voter counts; honest fsync, tested by pulling power
  mid-write; resource caps beside the builds; every answer says which node
  served it; a node comes back by restarting it, with no manual repair.

### Mimir

- Agrees with every item under "Impossible".
- **Agreement**: read sections "A" and "B" of Dannan's agreement design
  before sizing anything; without them a large value is shipped whole three
  times per change. Ranges were not discussed with Dannan; "a register per
  range [would be] the hottest contention point in the cluster".
- **Leases and "leaderless"**: its own reading is that a short, automatic,
  ballot-fenced lease has no designated node and no failover procedure and so
  is not outside "leaderless", but it is a leader for that range for its
  term, and it is the maintainer's to rule.
- **The levels**: for its own agreement commands (`SET NX`, `LPOP`) Mimir
  expects to recommend deciding before answering at every level, the level
  governing only durability: "the answer is certain, only durability
  trails", which it thinks the most useful relaxation for both projects.
- **Starting again** it would settle how the two classes meet on one key
  first, and never let the replication path and the persistence path both be
  the truth.
- **About to be written twice**: the level switch and its spelling; the dot
  and version vector; per-shard placement; a deterministic simulation harness
  with fault injection and a history checker, which belongs in Dannan's test
  fixtures and which Mimir would co-sign a proposal for.

### Macha Server (from its operating history; unverified items marked by it)

- **Power, not software, was the commonest outage.** Assume any node can
  vanish for ever and mid-write; never compile on a serving node; treat your
  own background work as a threat to the node (its ingest starved its own
  health checks for want of a disk-I/O reserve for control traffic).
- **A node that goes quiet but stays up is worse than a crash.** Control
  work never queues behind data work; a write that cannot complete is parked
  and woken only by the event that could change the answer, never retried in
  a loop.
- **Most outages were lifecycle and scheduling bugs, not data bugs**:
  wedged restarts, a repair loop asleep 31 hours on a bad timer, lost
  wakeups, stop hangs. Whoever constructs a thing owns its start, stop and
  destruction; every wait has a waker that can be named; every background
  loop reports progress.
- **Cancel and timeout paths are suspect**: its heap corruption was a
  timed-out request returning while a worker still wrote into the caller's
  buffer. The request owns its buffer.
- **Earned their keep**: the durability barrier with a failure latch;
  authenticated reads rather than "exists" checks; status matched by code,
  never by text.
- **Merge versus one answer.** Merging works where data is immutable and
  edits commute; the merge was cheap, and what cost at heal was everything
  that waited for it (GC, repair storms). Pace post-heal work as background
  or the heal becomes the outage. For SQL: mergeable data (append-only,
  node-minted keys) can be multimaster, a shared read-modify-write needs one
  agreed answer; "make the boundary explicit per table, not per session".
- **Build first**: the failure model as code, a deterministic simulator with
  crashes, partitions, slow disks, fsync errors, clock skew and a history
  checker. **Test hardest**: crash consistency at every write point, fsync
  failure, and "a node vanishes for ever during X" for every X. Absent nodes
  never block the side that can proceed. Recovery is code, not runbooks.
- **Laws a database breaks first**: "don't shoot yourself in the foot":
  compaction, index builds, catch-up and backups taking the disk or lock
  foreground needs; pace background by signal, never gate it on idle. Local
  always fast: keep level 2's cost confined to level 2. One definition per
  concept: decide once what "durable", "committed" and "available" mean.
  Unverified: lock-manager and deadlock-detector waits as the likeliest
  quiet breach of "control never waits".

### Lugh (design and a single-node build; not yet on Dagda)

- **Merging serves small versioned records, one record per write**: higher
  version then higher node id, a removal beats an edit, two same-named users
  from a split both kept and the name marked ambiguous. **It fails exactly
  where "at most once" matters** (TOTP steps, recovery codes), now registers
  in Dannan's agreement, failing closed on a minority. The audit chain is a
  node-local log, never merged, never rolled back, refused at its bound.
- **Expects of agreement**: the committed attempt's result; refusal on a
  minority within a deadline; forgetting by tombstone; acceptors admitted by
  the owner's capability, so no other hosted system votes in its registers.
- **If its data lived in Kvasir** (about 10,000 users, records under 4 KiB,
  a few writes a minute, every read from memory) it would need: a change
  feed (a commit seen elsewhere within about a second); conditional writes
  by version; unique indexes with a stated split behaviour; per-write
  durability with evidence; single-use semantics through agreement. It could
  relax multi-row transactions, serializable isolation and read freshness.
  It would not give up secrets sealed under key custody outside the
  database, and for that reason fits Dagda better and would not move.
- **MySQL's handshake**: TLS before any authentication, acting on nothing
  sent before the upgrade; no RSA alternative on plaintext; refuse
  `mysql_native_password` and `mysql_clear_password` outright and never
  answer an auth switch with a downgrade; the fast-auth cache in memory only
  and dropped on any principal change; `COM_CHANGE_USER` and
  `COM_RESET_CONNECTION` take the same path, back-off and audit; back-off
  per principal and per source before any hashing, identical refusals and
  constant work; compare principal ids as bytes, never through a collation;
  never log auth data or the typed name of an unknown user.
- **Warnings**: tests from the specification; no unbounded queue; every
  refusal says why with ids, never values; one truth per fact; review the
  sibling code you call.

### Hero Graphics

Declined: nothing in its work applies.

### Miach (a compiler that proves its own output)

- **No hardware failures in two days of long runs on two build Pis; what
  failed was everything around the job**: a hung ssh session, a watcher
  reading status files an earlier run left behind and reporting "done"
  early, a 3 GB machine among 15 GB ones. "Most node failures you will meet
  are the control plane lying": fence by generation numbers, never by "it
  stopped answering".
- **Correctness is defined by an independent reference, never by your own
  output**: a sequential model of SQL semantics or real MySQL as the oracle,
  and histories checked for serializability against it.
- **A claim of shared state is proven, not trusted**: state "replica R
  equals the agreed sequence up to index i" as an inductive invariant over
  the protocol's steps, in a model checker, before code.
- **Test the checker with seeded faults.** "A verifier that has never
  refused anything is unverified."
- **Make the gate cheap enough to run on every change**: deterministic
  simulation, one thread, injected clock, network and disk, a seed
  reproducing a failure exactly. Gates that take hours get skipped.
- **Watch the gap between the model and the real thing**: the bugs live
  where the simulator is more pessimistic or more optimistic than reality
  (syncs that lie, torn writes, clock jumps, partial partitions).
- **Refinement, not equality**: MySQL leaves much undefined (row order
  without `ORDER BY`, some implicit casts). Write the allowed differences
  down or differential testing drowns in noise.
- **As a user**: state that can be trusted after a crash; a clear answer to
  "is this committed?"; an honest error when the database does not know,
  not a timeout; and a view of what it is doing (why a node is out, the lag,
  which generation leads).

All twelve sessions are now accounted for (eleven answered, Hero Graphics
declined).
