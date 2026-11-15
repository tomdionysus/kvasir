# Acknowledgement levels: the group standard (2026-11-15)

Tom, 2026-11-15, to the Kvasir session, on how the acknowledgement mode is
named and switched in Kvasir and Mimir: names, not numbers, and "have
everyone concerned adopt the nomcleature, as a group standard". This is the
nomenclature, written by the Kvasir session for the sessions concerned. The
names and the setting's name are Tom's ruling; the rest restates his earlier
rulings to Kvasir and Mimir.

## The names

A level says how far a write has gone when the node answers the client.
Four, in order of strength; each is Tom's own word for the intent, or the
word used when he ruled it.

| Name | The node answers | An answered write is lost if |
|---|---|---|
| `ephemeral` | at once; the write is actioned after | the node stops first, or the cluster later refuses it |
| `persisted` | once one node's disk has the write: this node's when it can hold the copy, else the next in placement order | that disk is lost first, or the cluster later refuses it |
| `decided` | once a majority has agreed the write, in memory; disks after | a majority of the replicas lose power together |
| `replicated` | once a majority of the replicas have it on disk | never, while one majority's disks survive |

- **A level says how far the write has gone, not where it went** (Tom to
  Dagda, 2026-11-15, relayed by Dagda, asked with Macha's edge nodes that
  have no data disk and nodes that are full): `persisted` is "the disk that
  took it".
- **The level is the least the node waits for.** The remaining replicas
  catch up just after, and the answer's evidence says which replicas had the
  write. That is why "on every available replica" (Dagda's `durable`) is not
  a level: it promises no fixed number (Tom, 2026-11-15, confirming this
  reading by interactive question). What a two-node system such as Macha
  does when one node is down is for Dagda and Macha to settle with Tom.
- The setting is **`ack_level`** everywhere: the YAML key, the session
  setting and the per-command or per-statement form.
- The names replace the numbers. Where a document says level 0, 1 or 2 it
  means `ephemeral`, `persisted` and `replicated`; `decided` is the mode Tom
  added on 2026-11-15 and never had a number.
- Code, wire formats, Status, logs and documents use the names. An
  enumeration orders them as above, so "at least `decided`" is a comparison.

## What is each project's own

The names fix when the node answers. What a project does before answering
is its own ruling:

- **Kvasir**: at `ephemeral` and `persisted` the cluster decides after the
  answer, and a commit answered early can be refused later (rolled back
  whole, recorded, the session warned). At `decided` and `replicated` it is
  never refused later. The default is set in YAML with no built-in value.
  `ack_level` is a MySQL session variable (`SET SESSION ack_level =
  'decided'`), and per statement MySQL's hint
  (`COMMIT /*+ SET_VAR(ack_level='ephemeral') */`).
- **Mimir** (its ruling 23, as Mimir relayed it): an agreement command
  decides, then answers, at every level, so the level governs durability
  only; a merge write follows the table. Its command and per-command form
  are Mimir's to spell, with `ack_level` as the name.
- **Dagda** already uses three of these words, and two do not mean what the
  table above says. This is open and with Tom (Dagda put it to him directly,
  2026-11-15). From `dagda/store/distributed_store.h`, as Dagda reports it:
  - `ephemeral`: the same.
  - `persisted`: on disk on at least one node, this one tried first, a
    peer's disk counting when this node has no room. The table says "its own
    disk".
  - `replicated`: on disk on at least k nodes, k being the store's
    `write_copies` (Macha runs k = 1 at replication 2), not a majority.
  - `durable`: a fourth level of Dagda's, on disk on every available node of
    the replication set, which the table lacks.
  - `decided` is not Dagda's; it is agreement.
  **Ruled (Tom, 2026-11-15, to Kvasir by interactive question and to Dagda
  directly): Dagda changes to the standard.** `persisted` is this node's own
  disk and `replicated` a majority of the replicas on disk, in Dagda as
  everywhere. Dagda records the standard in the stack document when its
  change lands, and tells Macha's port first, since `replicated` at
  replication 2 becomes both nodes. Tom also confirmed that the levels are
  names, not numbers.
  **`durable` does not join the standard** (Tom: "No, the standard covers
  this. discuss with me."). Dagda keeps it beside the four for now. How the
  standard covers it is under discussion with Tom.

## Who is concerned

Mimir and Kvasir use the levels. Dannan's agreement (what an acceptor does
before it answers) and Dagda's log are what they are built on. Lir and Lugh
persist through the same pieces. The standard belongs once, in the stack
document (`../dagda/TODO/2026-11-03-stack.md`), which is Dagda's to edit;
until it is there, this file is the text.
