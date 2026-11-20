# Acknowledgement levels: the group standard (2026-11-15)

The maintainer, 2026-11-15, to the Kvasir session, on how the acknowledgement level is
named and switched in Kvasir and Mimir: names, not numbers, and "have
everyone concerned adopt the nomcleature, as a group standard". Written by
the Kvasir session for the sessions concerned. This file is the text; the
stack document (`../dagda/TODO/2026-11-03-stack.md`) points at it rather
than holding a copy (agreed with Dagda, 2026-11-20).

## The names

A level says how far a write has gone when the node answers the client: how
far, not where (the maintainer to Dagda, relayed by Dagda). Four, in order of strength.

| Name | The node answers | An answered write is lost if |
|---|---|---|
| `ephemeral` | at once; the write is actioned after | the node stops first, or the cluster later refuses it |
| `persisted` | once one node's disk has the write: this node's when it can hold the copy, else the next in placement order | that disk is lost first, or the cluster later refuses it |
| `decided` | once a majority has agreed the write, in memory; disks after | a majority of the replicas lose power together |
| `replicated` | once a majority of the replicas have it on disk | never, while one majority's disks survive |

- The setting is **`ack_level`** everywhere: the YAML key, the session
  setting and the per-command or per-statement form.
- Names replace numbers. Where an older document says level 0, 1 or 2 it
  means `ephemeral`, `persisted` and `replicated`; `decided` was added on
  2026-11-15 and never had a number.
- Code, wire formats, Status, logs and documents use the names. An
  enumeration orders them as above, so "at least `decided`" is a comparison.
- **The level is the least the node waits for.** The remaining replicas
  catch up just after, and the answer's evidence says which replicas had the
  write. So "on every available replica" is not a level: it promises no
  fixed number (the maintainer, confirming this reading).
- **A store may offer a subset of the levels.** A level it offers keeps
  exactly the meaning above. A level it does not offer is refused, naming
  the level, and never served quietly as another, even a stronger one.
  Where the reason is structural it says so, in the refusal and in its
  docs (Kvasir and Dagda, 2026-11-20). Kvasir 0.1.0, one node in memory,
  offers only `ephemeral`. Dagda offers no `decided` for content-addressed
  objects, which have no agreement step; a caller asks for `replicated`.

## What is each project's own

The names fix when the node answers. What a project does before answering is
its own ruling.

- **Kvasir**: at `ephemeral` and `persisted` the cluster decides after the
  answer, and a commit answered early can be refused later. At `decided` and
  `replicated` it is never refused later. The default is set in YAML with no
  built-in value. `ack_level` is a MySQL session variable and, per
  statement, MySQL's hint (`COMMIT /*+ SET_VAR(ack_level='ephemeral') */`).
- **Mimir** (its ruling 23, relayed by Mimir): an agreement command decides,
  then answers, at every level; the level governs durability only. Its
  command and per-command form are Mimir's to spell, with `ack_level`.
- **Dagda** (ruled by the maintainer to Kvasir and to Dagda): Dagda's store levels take
  the standard's meanings. Before, its `persisted` meant "at least one
  node's disk" and its `replicated` "k nodes' disks, k configured" (Macha
  runs k = 1). Dagda tells Macha's port first, since `replicated` at
  replication 2 becomes both nodes. Dagda's own fourth level, `durable` (on
  disk on every available node of the set), is not part of the standard and
  stays beside it for now; what a two-node system such as Macha does when one
  node is down is for Dagda and Macha to settle with the maintainer.
- **Dannan**: an acceptor that answers from memory is what makes a write
  `decided`, and one that syncs first is what makes it `replicated`.

## Who was told

Mimir, Dannan, Dagda, Lir and Lugh, by the Kvasir session, 2026-11-15.
