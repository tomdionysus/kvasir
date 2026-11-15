# Kvasir

[![CI](https://github.com/tomdionysus/kvasir/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/kvasir/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/kvasir/branch/develop/graph/badge.svg)](https://codecov.io/gh/tomdionysus/kvasir)
[![CodeQL](https://github.com/tomdionysus/kvasir/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/kvasir/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

The distributed relational database: MySQL-compatible, multi-master and
leaderless, with ACID transactions. Clients speak the MySQL protocol
to any node, and every statement works whichever node they talk to, with no
writer node, no leader and no central coordinator. In concept and resilience
it follows AWS's Aurora: the log is the database, storage is replicated by
quorum and heals itself, and a node's SQL state is soft. Kvasir is a hosted
system on Dannan, the stack's network layer, and persists in Dagda, the
stack's store.

## Status

Version 0.1.0, in development. Built so far: the library and its version,
`kvasir-node --version`, the MySQL wire protocol's codec (value encodings,
packet framing, the connection messages), `caching_sha2_password` and the
sign-in, the built-in accounts, and the SQL lexer and parser for the first
statements. It does not listen on a port, runs no SQL and is not yet a
database. The single node, the cluster
and persistence follow, in the order `TODO/ACTIVE.md` gives.

Compatibility is the MySQL 8.4 manual's documented behaviour, tested, with a
differential gate against a MySQL 8.4 server.

## Building

Debian 13 (or any system with GCC 14 or Clang 19, CMake 3.24, Boost 1.83,
OpenSSL 3, yaml-cpp and GoogleTest):

```
apt-get install g++ cmake ninja-build libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKVASIR_BUILD_TESTING=ON
cmake --build build
./build/kvasir_tests
./build/kvasir-node --version
```

Build options: `KVASIR_BUILD_TESTING`, `KVASIR_BUILD_FUZZING` (clang),
`KVASIR_WARNINGS_AS_ERRORS`. Dannan is fetched at its pinned tag.

## Roadmap

1. One node, in memory: the MySQL protocol, SQL, and multi-version
   transactions.
2. The commit protocol proven in Dannan's deterministic simulator.
3. The cluster: leaderless ACID commits by agreement through Dannan.
4. Persistence in Dagda: the log is the database, with backup and restore
   to a point in time.
5. The weaker acknowledgement levels, range splits, work shared across the
   cluster, and authentication through Lugh.

## Licence

GPLv3. See [LICENSE](LICENSE).
