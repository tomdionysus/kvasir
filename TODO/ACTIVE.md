# Kvasir - Active Work

Last updated: 2026-11-15. `COMPLETED.md` is the record. The design is
`TODO/2026-11-15-kvasir-design.md` (second draft): Tom's rulings 1 to 23, the
rulings relayed by other sessions, and what is still proposed. How it got
there: `2026-11-15-design-review.md` (the session's review and eleven
sessions' advice) and `2026-11-15-rationalised-design.md`. The level names
are `2026-11-15-ack-levels-standard.md`.

## Resume here

- Step 1 (the skeleton) is committed on `develop`. Gate: `tools/ci.sh
  release` on corvus-neptune, 1 passed. Not pushed: Tom creates
  `github.com/tomdionysus/kvasir` himself; when he says it exists, add it as
  `origin` and push `develop`.
- Next is step 2a, which waits on nothing: the MySQL packet reader and writer
  with its fuzz target; the handshake with TLS before authentication
  (Dannan's `Stream::start_tls`) and `caching_sha2_password`; `COM_QUERY`,
  `COM_PING`, `COM_INIT_DB`, `COM_QUIT`; text result sets; the lexer; and the
  statements a `mysql` client sends on connecting (`SELECT @@version_comment
  LIMIT 1` and its kin), served by `kvasir-node` on port 3306. Tests from
  the MySQL 8.4 manual first, watched to fail.
- The differential gate needs Oracle's MySQL 8.4 server on a build Pi
  (Debian packages only MariaDB). Not installed; installing it is a change
  to a shared machine, so post a note on the claim board and ask Tom first.
- `tools/ci.sh check` has not run anywhere: the build Pi used has no
  clang-format. Run it where one is installed before the next commit of code.

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends). Sources go by rsync to
  `/root/kvasir-work/kvasir` with Dannan beside them in
  `/root/kvasir-work/dannan`; `chown -R root:root` both after the sync, or
  git refuses the clone.
- Commit when a step's gate passes; push `develop` once the remote exists.
- Design questions go to Tom interactively, with a recommendation. He asked
  that advice be gathered and reconciled before he is asked.
- A ruling relayed by another session is recorded as relayed, with who
  relayed it, and confirmed with Tom when Kvasir's own work turns on it.

## With the other sessions

- **Dannan**: agreement is on its `develop` as 0.9. Kvasir reviewed the
  simulator design and the reconfiguration proposal; every point was taken.
  Kvasir owes its sequential model and its per-level loss rules for the
  simulator's checker once the shape is settled (step 3). The slot table
  with no root except at genesis is ruled (relayed by Dannan).
- **Mimir**: co-signed the simulator. Shares with Kvasir the level names and
  switch, and an atomic commit across replica sets, to be proposed to Dannan
  once, by whichever reaches it first.
- **Dagda**: adopting the level standard and recording it in the stack
  document. Its node-local log (slice 4) is paused for Macha's port; step 5
  waits on it. Dannan asked it which file contract the log writes through,
  so real Dagda can run in the simulator.
- Kvasir edits no sibling's tree.

## Waiting on Tom

- Replicas and quorum: three by default with the count and spread as
  settings (recommended), or Aurora's six over three domains.
- Stale reads: off by default, a session opts in, autocommit `SELECT` only
  (recommended).
- Before persistence: run in memory and say so (recommended), or refuse
  commits.
- The proposals under the design's "Acknowledgement levels", above all
  whether `decided` wins as `replicated` does.
- The SQL surface left until asked for (the replication protocol, XA,
  full-text and spatial indexes, loadable plugins).
- Installing MySQL 8.4 on a build Pi.

None of these blocks step 2a.

## Ordered work

1. The skeleton. Done.
2. One node, in memory: the protocol, SQL and transactions for the subset
   real drivers and tools use, with the differential gate. 2a next.
3. The commit protocol written down and passed through Dannan's simulator.
4. The cluster at `decided` and `replicated`. Waits on Dannan 0.9.0.
5. Persistence in Dagda, with backup and restore. Waits on Dagda's
   node-local log.
6. `ephemeral`, `persisted` and held commits; range splits and merges;
   pieces of a query on other nodes; the Lugh plugin; a change feed.
