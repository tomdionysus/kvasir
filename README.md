# Kvasir - The Distributed Relational Database

[![CI](https://github.com/tomdionysus/kvasir/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/kvasir/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/kvasir/branch/develop/graph/badge.svg)](https://codecov.io/gh/tomdionysus/kvasir)
[![CodeQL](https://github.com/tomdionysus/kvasir/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/kvasir/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Kvasir is a relational database for a cluster of trusted machines. It is a C++20 library
and a node binary that turn a set of nodes into one MySQL-compatible database: connect to
any node with any MySQL client, driver or tool, run any statement on any node, and the
cluster behaves as one server, with ACID transactions and no node in charge. It runs on
[Dannan](https://github.com/tomdionysus/dannan), the network, and persists in
[Dagda](https://github.com/tomdionysus/dagda), the store.

Kvasir is in development: the design is settled, and the MySQL protocol, sign-in and SQL
front end are built. It does not yet listen on a port or run SQL. "Status" below says
exactly what is built.

## Why it exists

Most systems keep what matters in a relational database, and most of those speak MySQL.
The usual way to run one is a primary and its replicas: every write goes to one machine,
and when that machine is rebooted, fills its disk or loses power, writes stop until a
replica is promoted, by a person or by a failover procedure that must itself be right.
Aurora shows the better half of the answer, storage replicated by quorum that heals
itself, but in its usual form it keeps one writer, and it runs only in its provider's
cloud.

The machines this stack runs on are the opposite of a data centre's: small nodes, Wi-Fi
between some of them, power that browns out under load, clocks that drift. On them a
single writer is a single point of failure that fails often.

Kvasir is the database written for this stack: MySQL, but fast, replicated and storage
scalable like Aurora, with work shared across the cluster; "a soft cluster MySQL that's
damn near impossible to properly take down with infra failures."

- **No writer node.** Every node takes every statement. There is no leader, no election
  and no failover procedure; a node lost is a node lost, and the others carry on.
- **ACID, as other major databases offer it.** A transaction is atomic, isolated and
  durable whichever node the client talks to. Two transactions never both commit writes
  to one row unseen by each other, and a committed transaction survives while a majority
  of its replicas' disks do.
- **MySQL compatible, not MySQL based.** Clients speak the MySQL 8.4 client/server
  protocol and SQL, and existing drivers, ORMs and tools connect unchanged.
  "Compatible" is defined by the MySQL 8.4 manual's documented behaviour, each piece
  tested, and by a differential gate against a real MySQL 8.4 server. Nothing of MySQL,
  MariaDB or Aurora is copied; the parser and everything else are written here.
- **The log is the database.** As in Aurora, only the log is written at commit; the
  stored form is built from it in the background, storage is replicated across failure
  domains and heals itself, and a node's SQL state is soft: a node comes back by being
  restarted.
- **On the cluster that is already there.** Membership, node identity, placement,
  agreement and the wire between nodes are Dannan's; what is stored is Dagda's. Kvasir
  adds only what makes it a database, and has no copy of anything below it.

## What it does, by design

The design is `TODO/2026-11-15-kvasir-design.md`; each statement there is marked as
ruled or proposed.

- **The MySQL front end.** The client/server protocol, version 10, over Dannan's
  listener. TLS before any authentication; `caching_sha2_password` only, its full path
  over TLS, with no RSA exchange and no downgrade to older methods. A built-in user list
  until the optional Lugh plugin. Every reader of client bytes (packets, SQL text) is
  bounded and has a fuzz target.
- **SQL.** A hand-written lexer and parser with MySQL 8.4's grammar, reserved words and
  operator precedence. The surface grows in a stated order: session statements; DDL;
  INSERT, SELECT, UPDATE, DELETE, REPLACE; types, expressions and functions; joins,
  grouping, subqueries, unions, common table expressions and window functions;
  transactions and locking reads; prepared statements; views; `information_schema` and
  SHOW; users and grants; stored programs, triggers and events.
- **Transactions.** REPEATABLE READ by default, READ COMMITTED, and SERIALIZABLE, over
  multi-version rows. A second writer to a row waits, as InnoDB's does, with MySQL's
  lock-wait and deadlock errors. When a commit cannot be answered in time the client is
  told the outcome is unknown, never that it failed, and can ask by transaction id.
- **The cluster.** A table's rows are split into ranges by primary key, each replicated
  to a configurable number of nodes across failure domains (one node alone is a working
  database). Each range is one register in Dannan's leaderless agreement; its commits
  go to whichever node committed on it last, which batches them, and any node can take
  a range over at any moment, safely, by ballot alone. A transaction across ranges is
  decided by one transaction record that any node can finish.
- **Storage in Dagda.** Each replica's decided commits live in a node-local Dagda log;
  in the background they are folded into immutable chunks, stored as sealed Dagda
  objects. Restore to a point in time and clones are claims on the same chunks.
- **Built to stay up.** Slow is treated as a failure, measured by latency and progress.
  Every call has a deadline, work is admitted by class (a client's statement before
  replication catch-up, before folding and backups), queues are bounded, and recovery is
  paced so that healing cannot brown out the survivors.

### Acknowledgement levels

Each session, or each COMMIT, chooses how far a commit has gone before the client is
answered. The names are a standard across the stack, shared with Mimir and Dagda:

| `ack_level` | The node answers | An answered commit is lost if |
|---|---|---|
| `ephemeral` | after its local checks; the cluster decides after | the node stops first, or the cluster later refuses it |
| `persisted` | once one node's disk has it; the cluster decides after | that disk is lost first, or the cluster later refuses it |
| `decided` | once a majority has agreed it, in memory | a majority of its replicas lose power together |
| `replicated` | once a majority has it on disk | never, while a majority's disks survive |

`ack_level` is a session variable, a per-statement hint
(`COMMIT /*+ SET_VAR(ack_level='ephemeral') */`) and a configuration key with no
built-in default: a node without it refuses to start. At every level the database never
shows part of a transaction or a broken constraint, and nothing is visible to another
session until the cluster has decided it. At `replicated` a transaction is ACID in full.
The weaker levels are a session's stated choice to be answered sooner, for data such as
telemetry, caches and session state.

## The stack

Kvasir is one of a stack of projects, each its own library and repository, written to
the same conventions. No code is duplicated between them: a piece needed by more than
one lives in exactly one.

```
   Lugh      Lir      Mimir      Kvasir      Macha, ...      hosted systems
     |         |        |           |            |
   Dagda: the store  (domains, sealing, objects, heads, logs)
     |
   Dannan: the network  (reachability, routes, channels, membership, agreement,
                         identity, placement, work classes, status)
```

- **[Dannan](https://github.com/tomdionysus/dannan)** is the network: reachability,
  routes and channels between nodes, membership, leaderless agreement, the hybrid clock,
  identity, placement, work classes, logging and the shared primitives (crypto, secure
  memory, the settings reader, the listener). Kvasir uses it for every byte between
  nodes and to its clients, and its deterministic simulator proves Kvasir's commit
  protocol before the cluster is built.
- **[Dagda](https://github.com/tomdionysus/dagda)** is the store: objects, claims, heads
  and node-local logs, sealed, at a durability level each write chooses. Kvasir keeps
  its commit logs, its stored chunks and its backups there.
- **[Mimir](https://github.com/tomdionysus/mimir)** is the stack's key-value database,
  Kvasir's sibling. They share their concepts and the acknowledgement-level standard.
- **[Lugh](https://github.com/tomdionysus/lugh)** is the directory and sign-on. To Kvasir
  it is one possible identity provider, through an optional plugin; Kvasir itself never
  links it. **[Lir](https://github.com/tomdionysus/lir)** is the event system, which the
  plugin and a future change feed use as any MQTT client would.

Hosted systems never address one another.

## How to use it

### Build

A C++20 compiler (GCC 14 or Clang 19), CMake 3.24 or later, Boost 1.83 or later (Asio),
OpenSSL 3, yaml-cpp, and GoogleTest for the tests. Dannan is fetched from `../dannan` at
the pinned release (0.8.2), so clone it beside this repository. On Debian 13:

```
apt-get install g++ cmake ninja-build libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKVASIR_BUILD_TESTING=ON
cmake --build build
./build/kvasir_tests
```

`tools/ci.sh` runs each check the CI runs: `check` (header blocks and formatting),
`release`, `clang`, `asan`, `tsan` and `coverage`. Build options are
`KVASIR_BUILD_TESTING`, `KVASIR_BUILD_FUZZING` (a clang with libFuzzer) and
`KVASIR_WARNINGS_AS_ERRORS`.

### Run a node

Not yet. `kvasir-node --version` prints the version; any other use says the node serves
nothing yet, rather than listening on a port it cannot answer. Running a node, a cluster,
configuration and the MySQL client's view of them are written here as they are built.

## Repository layout

```
kvasir/protocol/   the MySQL wire codec, packet framing, connection messages,
                   caching_sha2_password
kvasir/server/     the sign-in, the verifier cache and the built-in accounts
kvasir/sql/        the lexer, reserved words, syntax tree and parser
kvasir/include/    the public headers (the version)
kvasir/test/       GoogleTest, one binary (kvasir_tests)
kvasir/fuzz/       libFuzzer targets for the packet reader, lexer and parser
tools/             kvasir-node, and ci.sh
docs/              the principles and laws shared with the stack
TODO/              now and next (ACTIVE.md), the design, the level standard, and what
                   landed (COMPLETED.md)
```

The catalog, execution and transactions (`kvasir/catalog/`, `kvasir/exec/`,
`kvasir/txn/`) join as they are built. Conventions, shared with the stack:
[CONTRIBUTING.md](CONTRIBUTING.md).

## Status

Version 0.1.0, in development; no release is cut yet. Built and tested (163 tests, under
release and AddressSanitizer builds):

- The MySQL client/server protocol's codec: its integer and string encodings, packet
  framing with every length bounded, and the connection phase's messages, OK, ERR, EOF,
  column definitions and text rows.
- `caching_sha2_password` and the sign-in, as a state machine with no I/O: the greeting,
  the TLS request, the auth switch, the fast path against verifiers held only in memory,
  the full path over TLS, and MySQL's refusals in MySQL's words.
- The built-in accounts, with Argon2id credentials, checked with the same work whether
  or not a name exists.
- The SQL lexer, MySQL 8.4's 262 reserved words, and the parser for the statements a
  client sends as it connects: SELECT (a select list, FROM DUAL or tables, WHERE, ORDER
  BY, LIMIT), SET (variables, NAMES, CHARACTER SET), USE, BEGIN, START TRANSACTION,
  COMMIT and ROLLBACK, with MySQL's operator precedence.

Not yet compared with a MySQL 8.4 server: the tests follow the manual's text.

Next, in order (`TODO/ACTIVE.md`):

1. One node, in memory: values and expressions, the session and its variables, the
   command phase, then `kvasir-node` on port 3306 with a real `mysql` client, then the
   SQL and transactions real drivers and tools use, with the differential gate. The TLS
   upgrade waits on Dannan 0.8.4.
2. The commit protocol written down and passed through Dannan's deterministic simulator.
3. The cluster at `decided` and `replicated`.
4. Persistence in Dagda, with backup and restore to a point in time.
5. `ephemeral`, `persisted` and held commits; range splits and merges; work shared
   across the cluster; the Lugh plugin and a change feed.

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
