# Kvasir - Active Work

Last updated: 2026-11-15, at the end of the first session, written for a
session starting with no memory of it.

## Read, in this order

1. This file.
2. `TODO/2026-11-15-kvasir-design.md`: the design. The maintainer's rulings 1 to 31,
   the rulings other sessions relayed, and what is still proposed.
3. `TODO/2026-11-15-ack-levels-standard.md`: the four level names, a
   standard for the whole stack.
4. `TODO/COMPLETED.md`: what landed, with gate counts.
5. Mimir's design (`../mimir/TODO/2026-11-15-mimir-design.md`) and the
   stack document (`../dagda/TODO/2026-11-03-stack.md`), as `CLAUDE.md`
   asks, before work that touches the cluster or another project.
6. `TODO/2026-11-15-design-review.md` only when the sessions' measurements
   and warnings are wanted (the Pis' failures, Dagda's rates, the clocks).
   It reviews the first draft and is a record, not the design.

## Resume here

Step 2a, one node speaking the MySQL protocol, is in hand.

**Done and pushed** (`kvasir/protocol/`, 44 tests): the value codec, packet
framing, and the connection messages (greeting, the client's response and
SSLRequest, OK, ERR, EOF, authentication packets, column definitions, text
rows).

**Next, in order**, each with its tests written from the MySQL 8.4 manual
first and watched to fail:

1. `caching_sha2_password`: the scramble (XOR of SHA256(password) with
   SHA256(SHA256(SHA256(password)) and the 20-byte nonce)), the fast path
   against a verifier held in memory, the full path only over TLS. Through
   Dannan's crypto primitives, not OpenSSL directly, where Dannan has them.
2. The connection on Dannan's listener: greeting, TLS upgrade in place by
   `Stream::start_tls`, sign-in, then `COM_QUERY`, `COM_PING`,
   `COM_INIT_DB`, `COM_QUIT`. Mimir's `mimir/server/` is the model for using
   the listener. **`Stream::start_tls` (0.8.2 and Dannan's develop) does
   not fit MySQL**: it refuses the upgrade if any byte is waiting after the
   request, and a MySQL client sends its ClientHello straight after
   SSLRequest with no answer between. Asked of Dannan 2026-11-15; Dannan
   agreed the spelling `void Stream::start_tls_unanswered(std::function<
   void(std::string)> done)` (no answer, no waiting-bytes check, every other
   check kept), proposed to the maintainer for 0.8.4, not yet built. Until it lands,
   build the connection with sign-in refused before TLS and the TLS step
   behind one seam; then move the pin (`KVASIR_DANNAN_TAG`) to the first tag
   that carries it. Read SSLRequest with exact-length reads so nothing past
   it is consumed.
3. The lexer, with its fuzz target, and the statements a `mysql` client sends
   on connecting (`SELECT @@version_comment LIMIT 1` and its kin).
4. `kvasir-node` listening on port 3306, then a real `mysql` client against
   it by hand on a build Pi.

**Known and unverified**, to settle at the first run against a real client or
server:

- The tests use the manual's example bytes as recalled, not checked against
  a server.
- The response decoder refuses bytes after the last field it knows; a real
  client may send more (a compression level, for one).
- The fuzz target (`kvasir/fuzz/packet_fuzz.cpp`) has never been compiled:
  no build Pi tried has clang, and hosted CI's clang job does not build fuzz
  targets.

**Hosted CI has stopped running** (from the push of 7cc0940, 2026-11-15):
GitHub refuses every job with "recent account payments have failed or your
spending limit needs to be increased". The maintainer's to settle in the account's
billing. Until then the clang (thread-safety), ASan/UBSan and TSan gates
run only where a Pi can run them (no Pi has clang).

Before that, on hosted CI (`github.com/tomdionysus/kvasir`, private): every CI job
(check, release on x86-64 and arm64, clang, ASan/UBSan, TSan, coverage)
passes on `develop`. CodeQL's analysis completed but its `analyze` step
failed with "Resource not accessible by integration" reading its own
workflow run: the job lacked `actions: read`, added 2026-11-15. Whether the
upload then needs GitHub Advanced Security on a private repository is not
yet seen. Dependabot's two pull requests (actions bumps) failed `check`
only because they branched before the format fix; untouched, the maintainer's to
merge.

## How work is done

- No builds or test runs on the Mac. Every build is on the Pi build servers
  under a claim-board claim (`claims_list`, `claim_take` with the cores the
  `-j` needs, `claim_release` when it ends). Sources go by rsync to
  `/root/kvasir-work/kvasir` with Dannan beside them in
  `/root/kvasir-work/dannan`; `chown -R root:root` both after the sync, or
  git refuses the clone. Both exist on node-d (which has clang-format,
  so `tools/ci.sh check` runs there) and node-c (which has not).
  The remote tree is not a git checkout: `git add -A` there before `check`
  and `git reset -q` after.
- `clang-format -i` is run on the Pi and the formatted files copied back.
- Commit when a step's gate passes and push `develop`. No Claude attribution.
- Design questions go to the maintainer through the interactive question tool, with a
  recommendation first. He asked that other sessions' advice be gathered and
  reconciled before he is asked anything.
- A ruling relayed by another session is recorded as relayed, with who
  relayed it, and confirmed with the maintainer when Kvasir's own work turns on it.
- On this machine a hook blocks `cat`, `grep`, `sed`, `awk`, `head`, `tail`
  and `nl` over local files in the shell. Read files with the Read tool,
  search with basemind, and reach a sibling's branch with `git show
  <ref>:<path>` into `build/`.

## With the other sessions

- **Dannan**: agreement is on its `develop` as 0.9, with the simulator's
  first pieces. Kvasir reviewed the simulator design and the reconfiguration
  proposal; every point was taken. Kvasir owes its sequential model and its
  rule per level for which losses which faults excuse, for the simulator's
  checker, at step 3. The slot table with no root except at genesis is ruled
  (relayed by Dannan). Building `transfer()` found a safety gap, fixed with
  the maintainer's approval (relayed by Dannan, 2026-11-15): records are per slot, a
  transfer stays pending until a settle message follows its commit, and a
  change's names must share one slot. Read "Reconfiguration: the gap found
  in building it, and the fix" in Dannan's agreement design before the range
  map (moves, splits) is designed at step 3. Dannan has also taken Kvasir's
  ask to export its fuzz harness with the test fixtures (a 0.9 item, not
  started). Dannan's working tree may be on a feature branch: read
  its designs from `develop`.
- **Mimir**: co-signed the simulator. Shares the level names, and an atomic
  commit across replica sets, to be proposed to Dannan once by whichever
  project reaches it first.
- **Dagda**: adopting the level standard and recording it in the stack
  document. Its node-local log (slice 4) is paused for Macha's port; step 5
  waits on it. Dannan asked it which file contract the log writes through,
  so real Dagda can run in the simulator.
- Kvasir edits no sibling's tree.

## Waiting on the maintainer

Nothing blocks step 2. Not yet put to him:

- The default replica count (three proposed).
- The design's "Acknowledgement levels" items marked Proposed.
- The SQL surface left until asked for (the replication protocol, XA,
  full-text and spatial indexes, loadable plugins).
- Before the differential gate can run: the MySQL 8.4 container on one build
  Pi (ruled: Oracle's image under Docker, started by hand on a private port,
  after a note on the claim board). Not yet set up.

## Ordered work

1. The skeleton. Done.
2. One node, in memory: the protocol, SQL and transactions for the subset
   real drivers and tools use, with the differential gate. 2a in hand.
3. The commit protocol written down and passed through Dannan's simulator.
4. The cluster at `decided` and `replicated`. Waits on Dannan 0.9.0.
5. Persistence in Dagda, with backup and restore. Waits on Dagda's
   node-local log.
6. `ephemeral`, `persisted` and held commits; range splits and merges;
   pieces of a query on other nodes; the Lugh plugin; a change feed.
