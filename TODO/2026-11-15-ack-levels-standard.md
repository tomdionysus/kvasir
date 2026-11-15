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
| `persisted` | once its own disk has the write | that node's disk is lost first, or the cluster later refuses it |
| `decided` | once a majority has agreed the write, in memory; disks after | a majority of the replicas lose power together |
| `replicated` | once a majority of the replicas have it on disk | never, while one majority's disks survive |

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
- **Dagda**'s durability levels for an append or a put ("persisted",
  "replicated") already use two of these words with the same meaning; the
  standard takes them from there.

## Who is concerned

Mimir and Kvasir use the levels. Dannan's agreement (what an acceptor does
before it answers) and Dagda's log are what they are built on. Lir and Lugh
persist through the same pieces. The standard belongs once, in the stack
document (`../dagda/TODO/2026-11-03-stack.md`), which is Dagda's to edit;
until it is there, this file is the text.
