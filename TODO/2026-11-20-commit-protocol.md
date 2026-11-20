# Kvasir's commit protocol (step 3), written before its code

Proposed, 2026-11-20. The design (`TODO/2026-11-15-kvasir-design.md`, "Shape",
"Transactions", "Proving it") says the commit protocol is written down, Kvasir's
model and level rules supplied, and the protocol passed through Dannan's
simulator before the cluster is built. This is that text, for commits on one
range. A transaction across ranges waits on the stack's atomic commit across
replica sets, which is Dannan's (its stack plan's item 26, for 0.12.0; Dannan
writes its design first and Kvasir and Mimir confirm it before code). Kvasir
writes no cross-range protocol of its own; until it lands, such a
transaction is refused (1235), never committed partly.

## What is built on

- Dannan 0.10.0's agreement (`dannan/agree/agreement.h`): a range is one
  name in a slot of Kvasir's rows space; a commit batch is one `change` on
  it. `change` takes one name, which is why a cross-range commit waits for
  item 26.
- The acceptors' store: `agree::MemoryStore` now, which makes a commit
  `decided` (a majority holds it in memory). Dagda's durable AcceptorStore,
  under the same interface, makes it `replicated` (Dagda's 0.4.x or 0.5.0).
- Dannan's simulator and checkers (`dannan/test/fixtures/sim/`), linked as
  `dannan::sim_checkers`.

## The range's state and the one invariant

A range's value is its rows, each with the version (the commit id) that last
wrote it, and the set of commit ids it has applied within the dedup window.
Kvasir's local store (`txn::Store`) holds each node's copy as versions, as it
does today.

**Invariant**: a replica's state equals the agreed sequence of its range's
changes, applied in order, up to the position it reports. Nothing else ever
writes a replica.

## A commit on one range

1. **Statement time.** A writing statement takes its row locks at the
   range's proposer, as `txn::Store` takes them today, and reads each row it
   will write with a locking read. It records, for each row written, the
   version it read (its expected version) and the new row.
2. **Commit.** The coordinator (the client's node) sends the transaction's
   write set (row, expected version, new row) and its commit id (node,
   incarnation, sequence) to the proposer, which puts it in its next batch.
3. **The change.** The batch is one `change` on the range's name. Its
   function, given the current state, applies each transaction in batch
   order:
   - if the commit id is already applied, it is skipped and answered as
     committed (a retried change applies nothing twice);
   - else, if every written row's current version equals the expected
     version, its writes are applied and it is committed;
   - else it is refused whole (1213, "Deadlock found when trying to get
     lock; try restarting transaction"), and nothing of it is applied.
   The function is deterministic and idempotent by commit id, so it sets
   `Changed::repeatable`.
4. **Answer.** At `decided` the client is answered once the change commits;
   at `replicated`, once it commits on a store that syncs before answering.
   At `ephemeral` the client is answered after step 2's local checks and the
   batch is decided after; a refusal after the answer is recorded
   (`kvasir.refused_commits`), as the design says. `persisted` waits on
   Dagda's node-local log.
5. **Unknown.** An `unknown` outcome (the change may have taken effect) is
   settled by reading the range back: the commit id is in its applied set or
   it is not. The client of a `COMMIT` not settled in time is told the
   outcome is unknown and can ask by id (the design).

**Why this holds the one rule** ("two transactions never both commit writes
to one row unseen by each other"): every write is checked inside the agreed
change against the row's current version, so the second of two writers to a
row from one version is refused, whichever proposer ran which batch and
whatever locks were lost. The proposer's locks make the common case wait
rather than fail; safety rests on ballots and the version check alone, as the
design says ("Safety rests on ballots alone").

**Reads** that must be current go to the proposer, which confirms its ballot
with one majority round (`Agreement::read`); a snapshot is a version
position, so a reader never sees part of a batch.

## What the simulator proves

Each test runs real `Agreement`s and Kvasir's change function in Dannan's
`World`, under the faults it can inject: messages dropped, duplicated and
delayed; acceptors crashed keeping only what they synced, restarted, and
wiped; partitions; correlated kills (several nodes in one event). Every fault
is recorded in the `History`.

1. **Isolation.** Clients on several nodes run transactions over a few rows
   as list appends (Elle's encoding), with reads. `check_isolation` must
   find no anomaly at `snapshot` (REPEATABLE READ) and at `read_committed`,
   ordered by session.
2. **No lost or doubled write.** Every append answered as committed is in the
   final state exactly once, unless `loss_excused` excuses its loss at the
   level it was answered at, given the recorded faults. No refused or
   unanswered append is in it.
3. **The one rule.** Two writers of one row from one version: at most one
   commits, under every interleaving and fault the seeds reach, a proposer
   change mid-flight included.
4. **Unknown settles.** A change whose outcome was `unknown`, read back,
   gives one answer, and a retry never applies it twice.

Each runs over many seeds in kvasir_tests; a failing seed is kept as a test.

## Not in this step

Cross-range transactions (Dannan's item 26), ranges' splits, merges and moves, the
range map in slot registers, folding into Dagda chunks, and `persisted`.
