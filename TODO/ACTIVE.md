# Kvasir - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record. The design is
`TODO/2026-11-15-kvasir-design.md`: Tom's rulings ("Ruled", 1 to 6), what is
taken from Aurora, ACID, transactions without a leader, storage, and the order.

## Resume here

- Step 1 (the skeleton) is in the working tree on `develop`, uncommitted: Tom
  has not asked for a commit. Gate: `tools/ci.sh release` and `check` on a
  build server; the count is in `COMPLETED.md`.
- The design is a draft. Everything beyond the five rulings is Proposed, and
  no sibling session has reviewed it. Next: Tom's rulings on "Waiting on Tom",
  then the design goes to the Dannan, Dagda and Mimir sessions for review of
  what it asks of them.
- Then step 2a, which waits on nothing but the rulings marked "before 2a": the
  MySQL packet reader and writer with its fuzz target, the handshake and
  `caching_sha2_password`, `COM_QUERY`, `COM_PING`, `COM_INIT_DB`, `COM_QUIT`,
  text result sets, the lexer, and the statements a `mysql` client sends on
  connecting (`SELECT @@version_comment LIMIT 1` and its kin), served by
  `kvasir-node` on port 3306.

- 2026-11-15, later: Tom asked for the design to be checked and for every
  other session's advice. `2026-11-15-design-review.md` holds the session's
  review and ten sessions' answers (Miach and Hero Graphics outstanding).
  `2026-11-15-rationalised-design.md` reconciles them into a revised
  proposal and lists what goes to Tom. Tom's instruction: wait for all the
  advice and rationalise before asking him anything. Done: all twelve are
  accounted for, and Tom ruled four things on the rationalised document
  (design rulings 8 to 11: the sticky proposer is leaderless; a fourth mode
  "decided, disks trail"; writers wait as InnoDB; the simulator once, in
  Dannan, jointly with Mimir). The simulator proposal went to the Dannan
  session, copied to Mimir. Next: rewrite the design document from the
  rationalised one, and put "Still for Tom" to him.

- Relayed by the Dannan session as Tom's ruling to it (2026-11-15), not
  heard by this session from Tom: anything that can use consensus or
  agreement waits for Dannan's agreement primitive, which leads 0.9.0 after
  0.8.3 is cut; build no consensus, no lease standing in for consensus and
  no workaround meanwhile. For Kvasir: the commit path, the sticky proposer
  and any range lease wait on it. This matches the order already proposed
  (the cluster step waits on 0.9.0); the single node (step 2) needs none.

- Dannan's simulator design is `../dannan/TODO/2026-11-15-simulation-harness.md`
  (in 0.9.0 at Tom's ruling to Dannan; agreement is on Dannan's develop as
  0.9). Kvasir reviewed it 2026-11-15 and asked for: a checker that takes
  the isolation level (snapshot permits write skew, so a serial-order search
  alone would refuse correct histories); a resolution event for commits
  answered early; fault events in the history so a loss is judged against
  what each level allows; correlated kills; SimDisk under real Dagda;
  roster, routes and placement in the world; a simulated worker pool; and
  generators that replay across compilers. Kvasir owes its sequential model
  and its per-level loss rules once Dannan settles the shape.
- The level names are the stack's standard: Tom ruled that Dagda changes to
  the standard's meanings, and confirmed names, not numbers
  (`2026-11-15-ack-levels-standard.md`). `durable` does not join it; how the
  standard covers it is being discussed with Tom.
- Dannan's reconfiguration proposal (its develop branch,
  `TODO/2026-11-15-agreement-design.md`, "Reconfiguration, proposed in
  full"): `transfer()` moves a name between sets by a joint change, and a
  never-touched name takes its set from an agreed map on a root set. It fits
  Kvasir: the range map as registers on the root, moves as transfers, splits
  as a map intent then one change on the range's set. Reviewed 2026-11-15;
  asked of Dannan: that the serving refusal carry the new set (so existing
  ranges survive the root being unreachable), the "map first, then touch"
  rule, `transfer()` of a large value by version after catch-up, and the
  fast path invalidated by a transfer. **For Tom**: the root set is
  operator-declared and changed only by an operator, which sits badly with
  "no designated nodes" and "near impossible to take down"; proposed instead
  that it be declared at genesis only and then move by `transfer()` like
  any other register. Touches Mimir and Dannan too. Dannan agreed and
  rewrote its design that way (its commit 00fbc23) and answered the four
  asks yes. Put to Tom interactively; he ruled for no option and wrote:
  "Dannan, and it's predecessor code in Macha, were explicitly written to
  avoid this. What went wrong?" The root is unsettled in every form until he
  rules; Dannan and Mimir were told. Dannan then proposed a form with no
  root (its design, "Open: waits on Tom"): a slot table fixed for the
  cluster's life; an untouched slot's set is the genesis members, a constant
  carried with the cluster's identity; each slot's set recorded in its own
  acceptors and moved by `transfer()`. Kvasir's range map would be slot
  registers keyed by table. Kvasir supports it and asked that every slot be
  touched at founding and that a late joiner find a moved slot by a soft
  hint, so no genesis member need stay alive. Tom's to rule; Dannan is
  answering him directly.
- The remote: Tom creates `github.com/tomdionysus/kvasir` himself. When he
  says it exists, add it as `origin` and push `develop`.

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends). Sources go by rsync to
  `/root/kvasir-work/kvasir` with Dannan beside them in
  `/root/kvasir-work/dannan`; `chown -R root:root` both after the sync, or git
  refuses the clone.
- Commit and push only at Tom's word. There is no remote yet.

## Waiting on Tom

Ruled since this list was written (design rulings 6 to 15): the
acknowledgement levels and their names, the sticky proposer, waiting writers,
the simulator, the tagline, MySQL 8.4 with Oracle's server, and commits per
gate with a private remote. Of the items below only these remain open:
replicas and quorum, stale reads on a minority side, running before
persistence, and the SQL surface left until asked for. Also open: creating
the private GitHub repository (it does not exist; the session has not
created it), and installing MySQL 8.4 on a build server.

Each with the session's recommendation.

Before 2a:
- **The tagline**, which is in every file's header: "Kvasir - The Distributed
  SQL Database" is in use.
- **The compatibility reference**: MySQL 8.4 LTS (recommended), and for the
  differential gate a MySQL 8.4 server on one build server. Debian 13 packages
  MariaDB, not MySQL, so this means Oracle's arm64 packages or a container
  there.
- **Commits and the remote**: whether Kvasir commits when a step's gate passes
  and pushes `develop` to a private `github.com/tomdionysus/kvasir`, as Mimir
  does.

Before step 3:
- **Blocking writes**: a second writer of a row waits for the first
  transaction, as InnoDB's row locks do (recommended, at one agreement round
  per writing statement), or every transaction runs optimistically and the
  loser is refused at commit with error 1213, as Aurora's multi-master did
  (cheaper, and unlike MySQL to an application that does not retry).
- **Replicas and quorum**: 3 replicas with a majority of 2 by default
  (recommended, as Mimir and Dagda's log), with the count and the
  failure-domain spread as settings, or Aurora's 6 copies over 3 domains with
  4 to write as the default.
- **Stale reads on a minority side**: off by default and a session opts in,
  for autocommit `SELECT` only (recommended), since ruling 5 leaves no room
  for them inside a transaction.
- **A merge class**: none (recommended). A table whose writes merge after a
  split, as Mimir's merge commands do, would sit outside ruling 5 and would be
  an explicit, per-table choice if it were ever wanted.

Before step 5:
- **Before persistence**, whether Kvasir runs in memory and says it is not
  durable (recommended) or refuses commits as Mimir refuses its durable levels.
- **The acknowledgement levels** (ruling 6) are ruled in the design: what
  each level answers after, level 1 async, local checks at level 0, the
  default from YAML only, visibility, late refusal, the minority side, and
  level 2 winning. Still open: the spelling of the switch (proposed: session
  variable `kvasir_ack_level`, MySQL's `SET_VAR` hint per statement, a YAML
  key), which Mimir asks be ruled once for both projects; and the small
  proposals listed under the rulings. The design's "earlier reading"
  subsection is superseded and is to be removed when the section is next
  rewritten. Mimir is to be told what Tom ruled, since its level 1 and
  Kvasir's now match and its design says "a majority have it" for level 2.

Later:
- **The SQL surface left until asked for**: the replication protocol, XA,
  full-text and spatial indexes, loadable plugins (the design's
  "Compatibility").

## Ordered work

1. The skeleton. Done, uncommitted.
2. One node, in memory: the protocol, SQL, the catalog, types, the executor,
   multi-version transactions, with the differential gate.
3. The cluster by agreement. Waits on Dannan 0.9.0's agreement primitive and
   hybrid clock.
4. Range splits and merges, placement changes, catch-up.
5. Persistence in Dagda: the log, folding, heads, the cache. Waits on Dagda's
   contract v2.
6. Restore to a point in time and clones; then the Lugh plugin.
