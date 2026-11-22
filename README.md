# Kvasir - The Distributed Relational Database

[![Release](https://img.shields.io/github/v/tag/tomdionysus/kvasir?sort=semver&label=release)](https://github.com/tomdionysus/kvasir/tags)
[![CI](https://github.com/tomdionysus/kvasir/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/kvasir/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/kvasir/branch/develop/graph/badge.svg)](https://codecov.io/gh/tomdionysus/kvasir)
[![CodeQL](https://github.com/tomdionysus/kvasir/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/kvasir/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Kvasir is a MySQL-compatible relational database. Any MySQL 8.4 client, driver or
tool connects to a Kvasir node and speaks SQL to it as it would to MySQL. Kvasir is
written to become a leaderless, multi-master cluster with ACID transactions, on
[Dannan](https://github.com/tomdionysus/dannan), the network layer, and
[Dagda](https://github.com/tomdionysus/dagda), the store.

**Kvasir is a prototype.** It is not ready for any data you need to keep, and it will
stay a prototype until it has a durable store and has been tested on a cluster.
Today it is one node, in memory: it keeps nothing on disk and does not yet join other
nodes, so everything is lost when the node stops. It therefore starts only at the
`ephemeral` acknowledgement level, whose promise is exactly that.

## Build

Kvasir is not published as a package; build it from source. It needs a C++20
compiler (GCC 14 or Clang 19), CMake 3.24 or later, Boost 1.83 or later, OpenSSL 3
and yaml-cpp, and GoogleTest for the tests. Dannan is fetched at its pinned
release from a checkout beside this one (`../dannan`); to fetch it from GitHub
instead, add `-DKVASIR_DANNAN_REPOSITORY=https://github.com/tomdionysus/dannan.git`.
The tests drive a node with a real MySQL client library, which the first test
build downloads from Oracle and builds (about 480 MB of source, needing
pkg-config and the headers of libtirpc and ncurses);
`-DKVASIR_MYSQL_CLIENT_TESTS=OFF` leaves those tests out. On Debian 13:

```
sudo apt install g++ cmake ninja-build pkg-config libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev libtirpc-dev libncurses-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKVASIR_BUILD_TESTING=ON
cmake --build build
./build/kvasir_tests
```

The node is `build/kvasir-node`.

## Run a node

A node needs a TLS certificate and key (clients sign in only over TLS), at least one
account, and an acknowledgement level, which has no built-in default.

1. Make a credential for each account. The password is read from standard input:

   ```
   echo 'a password' | kvasir-node hash-password
   ```

2. Start the node with the certificate chain and key, the account and the level:

   ```
   kvasir-node --tls-certificate node.pem --tls-key node.key --ack-level ephemeral \
               --users "root:<credential>" [--listen-host 0.0.0.0] [--listen-port 3306]
   ```

3. Connect with any MySQL client:

   ```
   mysql -h <node> -u root -p --ssl-mode=REQUIRED
   ```

Every setting is a command-line flag, a key in the settings file (`--config PATH`,
`$KVASIR_CONFIG`, or `/etc/kvasir/config.yaml`), a key in `secrets.yaml` beside it,
or a `KVASIR_*` variable; `docs/man/kvasir-node.1` lists them. SIGINT or SIGTERM stops the
node.

## What a node does

Kvasir is "MySQL compatible, not MySQL based": the MySQL 8.4 Reference Manual and its
client/server protocol are the specification, and nothing of MySQL is copied. Each
behaviour is tested, and a differential gate replays 966 statements, among them what
Connector/J, Python's and Go's drivers send as they connect and the queries behind
Connector/J's metadata calls, and compares every column's type, length, flags,
value and error with what a MySQL 8.4.13 server answered: all 966 match.
Connector/J's `getTables` and `getColumns` against a running node answer as
MySQL's do, field for field.

- **Connections.** The MySQL client/server protocol over TLS, `caching_sha2_password`
  only, accounts from the settings with Argon2id credentials, and a back-off on
  failed sign-ins.
- **Statements.** SELECT over one table (DISTINCT, WHERE, GROUP BY, HAVING, ORDER BY,
  LIMIT), INSERT, UPDATE and DELETE (with ORDER BY and LIMIT), CREATE and DROP of
  databases and tables, SHOW DATABASES, TABLES, WARNINGS and ERRORS,
  `information_schema`'s tables that drivers read, SET (variables, NAMES, CHARACTER
  SET), USE, and prepared statements.
- **Transactions.** BEGIN, START TRANSACTION, COMMIT and ROLLBACK at REPEATABLE READ
  and READ COMMITTED over multi-version rows, with row locks, lock waits, the lock-wait
  timeout and deadlock detection, answered with MySQL's errors.
- **Types and expressions.** MySQL's integer, decimal, float and string types, keys
  and AUTO_INCREMENT; MySQL's result types, exact decimals, overflow, division and
  conversion rules.
- **Functions.** The aggregates (COUNT, SUM, AVG, MIN, MAX, the bit and variance
  families), the flow control functions, and the common string and number functions.
- **Collations.** utf8mb4_0900_ai_ci and _as_cs from Unicode's UCA 9.0.0 table, the
  general_ci collations, and the binary ones, under MySQL's coercibility rules.

Not yet built, and refused with MySQL's "not supported" error: joins, subqueries,
unions, views, stored programs, users and grants, and any function not listed above.

## Where it is going

The design is `TODO/2026-11-15-kvasir-design.md` and the plan, in order, is
`TODO/ACTIVE.md`. In short: the node's state in Dagda; the commit protocol proven in
Dannan's deterministic simulator; then the cluster. A table's rows are split into
ranges, each replicated across failure domains and decided by Dannan's leaderless
agreement, so that every node takes every statement with no leader and no failover.

Each session will choose how far a commit has gone before it is answered, a standard
shared with Mimir and Dagda:

| `ack_level` | The node answers | An answered commit is lost if |
|---|---|---|
| `ephemeral` | after its local checks | the node stops first (the only level Kvasir keeps today) |
| `persisted` | once one node's disk has it | that disk is lost first |
| `decided` | once a majority has agreed it, in memory | a majority of its replicas lose power together |
| `replicated` | once a majority has it on disk | never, while a majority's disks survive |

## The stack

Kvasir is one of a stack of projects, each its own repository, sharing one set of
conventions and no duplicated code:

- [Dannan](https://github.com/tomdionysus/dannan), the network layer: reachability,
  membership, leaderless agreement, identity and the shared primitives.
- [Dagda](https://github.com/tomdionysus/dagda), the store.
- [Mimir](https://github.com/tomdionysus/mimir), the key-value database, Kvasir's
  sibling.
- [Lugh](https://github.com/tomdionysus/lugh), the directory and sign-on.
- [Lir](https://github.com/tomdionysus/lir), the event system.

## Development

`tools/ci.sh` runs one step at a time: `check` (header blocks and formatting),
`release`, `clang`, `tidy`, `coverage`, `fuzz` and `deb`, which CI runs on each push,
and `asan` and `tsan`, run before a release. `deb` builds a Debian package
(`docs/packaging.md`) to keep the packaging working; no package is published while
Kvasir is a prototype. Build options are
`KVASIR_BUILD_TESTING`, `KVASIR_BUILD_FUZZING` (clang with libFuzzer),
`KVASIR_WARNINGS_AS_ERRORS`, `KVASIR_SYSTEM_DANNAN` (the packaging build), and
for the tests `KVASIR_MYSQL_CLIENT_TESTS`, `KVASIR_MYSQL_CLIENT_ROOT` (where the
MySQL client libraries are built once and found after) and
`KVASIR_MYSQL_CLIENT_JOBS`.

Conventions: [CONTRIBUTING.md](CONTRIBUTING.md). Everyone taking part follows the
[Code of Conduct](CODE_OF_CONDUCT.md).

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`. Unicode's
data files in `kvasir/data/unicode/` are under the Unicode License v3, beside them.
