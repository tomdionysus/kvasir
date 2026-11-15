# Kvasir: the design rationalised against the advice (2026-11-15)

The Kvasir session's reconciliation of its draft (`2026-11-15-kvasir-design.md`),
its own review and the sibling sessions' advice (`2026-11-15-design-review.md`)
against the maintainer's goal. Everything here is **Proposed**. It changes none of the maintainer's
rulings; where the advice pulls against a ruling, that is listed under
"For the maintainer" and not acted on. All twelve sessions are accounted for: eleven
answered and Hero Graphics declined.

## The goal, and what it is taken to mean

The maintainer: "MySQL but fast, replicated, storage scalable like Aurora, and even with
the possibility of sharding work across the cluster. In short, a soft cluster
MySQL that's damn near impossible to properly take down with infra failures.
The design will be a compromise, and that's where what is useful comes in."

- **MySQL**: the wire protocol and the SQL that real drivers, ORMs and tools
  issue, behaving as MySQL 8.4 documents. A defined subset first, not every
  statement.
- **Hard to take down** is the first property, ahead of speed. The failures
  the sessions actually saw are brownouts under load, nodes that are slow
  rather than dead, Wi-Fi at about 0.5 MB/s between Pis, partial partitions
  in which two nodes both believe they act, and clocks 5 to 15 ms apart with
  no bound. The design is for those, not for clean crashes.
- **Fast** is relative to that hardware. A commit acknowledged at level 2
  costs at least one round trip to a majority and one disk sync on each, on
  links whose latency spikes to tens of milliseconds, and the replicated
  volume is capped by the slowest link. Speed comes from batching many
  commits into one decision, from reading locally where that is sound, and
  from the lower levels, not from skipping the majority.

## What changes from the draft

1. **One truth.** The draft replicated every commit twice. Now: a commit is
   ordered and replicated once, by Dannan's agreement, among a range's
   replicas. Each replica's durable state (the agreement's acceptor state,
   which carries the decided commits) is a node-local Dagda log on that
   replica, with no rollback. Dagda's replicated log is not used for commits.
   (Dannan, Dagda, Lir, Mimir and Macha all advised this.)

2. **The unit of agreement is the range's commit sequence, with a sticky
   proposer.** A register per row means many agreements per transaction; an
   unmanaged register per range duels. Proposed between the two: each range
   has one register, the next position in its commit sequence, and nodes send
   a range's commits to whichever node committed on it last. That node keeps
   its ballot (Dannan's one-round fast path), batches every commit that
   arrives within a 10 to 20 ms tick into one decision, and so pays one round
   trip and one sync per replica per tick, however many transactions are in
   it. Any node may take a range over at any instant by out-balloting, which
   costs two round trips once; there is no term, no timer and no election,
   and safety rests on ballots alone, never on a clock. A node that is slow
   is taken over from, as a dead one is, with backoff so two nodes do not
   trade a range back and forth. Whether this is inside "leaderless" is
   the maintainer's to rule (see "For the maintainer").

   Checked against Dannan's agreement design
   (`../dannan/TODO/2026-11-15-agreement-design.md`) as written today, by
   reading, not by test. It fits without asking Dannan for anything new: the
   range is one register whose value is the range's state; each batch of
   commits is a delta against the last committed ballot (its ask A); the
   sticky proposer is its fast path, "a proposer that committed a name last
   caches its ballot"; the replica set is part of the agreed state, so two
   nodes with different rosters cannot both commit; a replica that restarts
   without its state does not vote until it has caught up; and the store may
   sync before answering, group-committed on a node-local Dagda log. Dannan
   itself adds no batching delay, so the tick is the store's group commit.
   the maintainer's ruling relayed by Dannan (nothing that can use agreement is built
   before the primitive, and no lease stands in for it) is kept: this uses
   the primitive and nothing else.

3. **Conflicts are settled in memory at the range's proposer.** Because one
   node is sequencing a range at any moment, it can hold that range's row
   locks and read marks in memory: a second writer of a row waits as under
   InnoDB (error 1205 at `innodb_lock_wait_timeout`, 1213 on a cycle), at the
   cost of one forwarded call and no agreement round. The lock table is soft:
   when a range changes proposer, transactions in flight on it are refused
   with 1213 and retried by the client. No intent is replicated before
   commit. A transaction on several ranges commits through one transaction
   record, as in the draft.

4. **No correctness rests on clocks.** Snapshots and commits are ordered by
   Dannan's hybrid clock used as a logical clock. A range's proposer never
   commits at or below a timestamp it has served a read at, and tells a
   reader its own floor, so a snapshot across ranges is consistent with no
   bound on skew. A read that must be current goes to the range's proposer,
   which confirms it still holds its ballot with one majority round; a
   session that has opted in reads a slightly old snapshot from any replica
   with no round at all. Time-based read leases, which would save that round,
   are left out until skew is measured on the wire. What this does not give:
   a transaction may not see a commit that finished milliseconds earlier on
   another node unless it comes from the same session, which carries its own
   last commit timestamp forward.

5. **Isolation.** `REPEATABLE READ` is a snapshot; `READ COMMITTED` a
   snapshot per statement; `SERIALIZABLE` adds range read marks at the
   proposer, which stops phantoms without a register per gap.

6. **Storage in Dagda's natural shape.** Decided commits are folded, out of
   the commit path, into immutable chunks of a few MiB stored as objects with
   claims. The list of chunks per range (the manifest) lives in one head
   space for all ranges, committed once per fold, never per SQL commit: a
   head commit manages under a hundred a second per node, and a node may
   advertise only 64 spaces. Restore points and clones are claims on the same
   chunks, claimed before the source can release them. Every byte goes
   through Dagda, sealed.

7. **Held commits are remembered.** Each commit answered early carries an id
   (node, incarnation, sequence). Decided and refused ids are kept for a
   stated window, so a held commit put again after a heal or a restart is
   never applied twice; only its origin, or the range's proposer once the
   origin has left the roster, finishes it. The hold is bounded and gives
   way in a stated order: shed level 0, refuse level 1, refuse new
   connections; never crash.

8. **Allocators are fenced.** `AUTO_INCREMENT` blocks are reserved through
   agreement, so two nodes never issue one value. A restore or a clone moves
   every allocator past anything the original could have issued before it
   takes a write.

9. **Authentication, from Lugh's advice.** TLS before any authentication,
   through Dannan's `Stream::start_tls`; nothing sent before the upgrade is
   acted on. `caching_sha2_password` only; `mysql_native_password` and
   `mysql_clear_password` are refused, with no setting, and an auth switch
   never downgrades. The fast-path verifier is held in memory only and
   dropped on any change to the principal. Names are compared as bytes.

## Built to stay up

- Three replicas of a range by default, an odd count, placed across power
  domains; a failure domain is a power supply as much as a host.
- **Slow is a failure**: health by latency and progress, not heartbeats;
  under-voltage and throttling read from the node and shown on Status.
- A deadline on every call; refusal at the edge and backpressure by work
  class; no unbounded queue. Every answer that involved more than one node
  says which answered and what failed first.
- **Recovery is paced and budgeted.** A replica catching up, a fold, an
  index build and a backup are loader or speculative work, paced by signal
  and never gated on idle, so that healing after a failure cannot brown out
  the survivors or starve foreground statements (law 4, and the commonest
  way a database breaks it).
- A node comes back by being restarted, with no manual repair. Node-local
  state is never restored from an older copy. A failed sync latches and the
  replica re-derives from its peers; it never continues as if durable.
- Any node serves any statement, so there is nothing to fail over. MySQL has
  no redirect: clients are given several hosts or one name over all nodes,
  and a connection whose node dies is lost with its session state and
  reconnects.
- A node need not hold data. Placement is by capacity, so a small node can
  serve SQL and hold few ranges or none.
- Formats on disk and on the wire are versioned, old ones read and unknown
  ones refused; an upgrade is one node at a time and is tested from the
  previous release.
- Backup and restore to a point in time come with persistence, before any
  further replication feature: replication copies a mistaken `DELETE`
  faithfully.

## Proving it (from Miach, Macha, Dagda, Dannan and Mimir)

- **The reference is independent.** SQL behaviour is judged against a MySQL
  8.4 server (the differential gate), and the commit protocol's histories
  against a sequential model by a serializability checker, never against
  Kvasir's own output.
- **Allowed differences are written down**: where MySQL leaves the result
  undefined (row order without `ORDER BY`, connection ids, timings), the
  gate compares up to that list and nothing else.
- **The protocol's invariant is stated before code**: a replica equals the
  agreed sequence of its range up to the position it reports, held by every
  step.
- **Deterministic simulation**: one thread, injected clock, network and
  disk, a seed that reproduces a failure exactly, cheap enough to run on
  every change. Faults: a process killed between any two writes, a node gone
  for ever during each operation, partial partitions, slow disks, a sync
  that fails or lies, torn writes, clock jumps.
- **The checker is itself tested** with protocols broken on purpose, which
  it must refuse with a counterexample.
- **The gap to the real thing is closed on hardware**: power pulled
  mid-write on a Pi, since brownouts are the failure that happens.
- **"Is this committed?" always has an answer.** Every transaction has an
  id; when a `COMMIT` cannot be answered in time the client is told the
  outcome is unknown, not that it failed, and can ask by id afterwards.

## Sharding work across the cluster

Possible, and later. The pieces exist in Dannan (calls, work classes,
budgets, placement). The rule the links impose: run the work where the data
is and ship results, never rows. A statement's scan is split by range into
pieces (filter, projection, partial aggregate, limit), each run at a replica
of its range, keyed by query and piece so a duplicate is dropped, with a
deadline, the first good answer taken and the rest cancelled; large scans in
the loader class. Joins that would move rows between nodes are not attempted
at first.

## The levels, as ruled, and what the advice says about them

Nothing here changes rulings 6 and after. Observations for the maintainer:

- Mimir, Macha and Corvus LoRa independently point at the same two things.
  First, the most useful relaxation is "decided by the cluster before the
  answer; only the disk writes trail", which the three ruled levels do not
  include. Second, the boundary between data that may be lost and data that
  may not is better drawn per table than per session, so that a ledger
  cannot be written carelessly at level 0.
- Holding commits through a split is useful for insert-only tables with keys
  the node can mint, and mostly produces refusals for updates to shared rows.
- Lugh, the nearest thing on the stack to a customer, would want a change
  feed (a commit seen elsewhere within about a second) and conditional
  writes by version more than it would want multi-row transactions.

## Order, revised

1. The skeleton. Done.
2. One node, in memory: the protocol, SQL and transactions for the subset
   real drivers and tools use, with the differential gate.
3. The commit protocol written down and passed through a deterministic
   simulator with injected crashes, partitions, slow disks, sync failures
   and clock skew, and a history checker. Dannan and Mimir both want that
   harness built once, in Dannan's test fixtures.
4. The cluster at level 2 only: ranges, sticky proposers, the transaction
   record, replicas in memory. Waits on Dannan 0.9.0.
5. Persistence: acceptor state in node-local Dagda logs, folding into
   chunks, the manifest head, backup and restore to a point in time. Waits
   on Dagda's node-local log.
6. Levels 0 and 1 and held commits; range splits and merges; then pieces of
   a query on other nodes, the Lugh plugin, and a change feed.

Levels 0 and 1 move after a correct level 2, on Louhi's and Macha's advice:
right first, and keep level 2's cost confined to level 2.

## Ruled by the maintainer on this document (2026-11-15, by interactive question)

1. **A sticky proposer is leaderless**: items 2 and 3 above stand. Timed
   read leases were not chosen.
2. **A fourth acknowledgement mode is added, "decided, disks trail"**;
   levels 0, 1 and 2 stay as ruled. A table setting its own lowest level was
   not chosen.
3. **The second writer of a row waits, as InnoDB does.**
4. **The simulator is built once, in Dannan, proposed jointly with Mimir.**

## Still for the maintainer

- The tagline; MySQL 8.4 as the reference; commits and the remote.
- The spelling of the level switch and the name or number of the fourth
  mode, as one ruling for Kvasir and Mimir. Mimir's ruling 23 (relayed by
  Mimir) makes its agreement commands decide before answering at every
  level, by design unlike Kvasir's levels 0 and 1; Kvasir's fourth mode is
  the same guarantee.
- Mimir was also ruled atomic multi-key commands across slots, which needs a
  commit across replica sets on Dannan's agreement: the same piece as
  Kvasir's transaction record, to be written once (rule 3).
