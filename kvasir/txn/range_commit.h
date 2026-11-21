//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "dannan/agree/caspaxos.h"
#include "dannan/net/codec.h"

// A commit on one range (TODO/2026-11-20-commit-protocol.md): a range is one
// name in Dannan's agreement, and a batch of transactions is one change on it.
// Inside the change each transaction is checked against the versions its rows
// were read at, and applied whole or refused whole; a transaction already
// applied, by its commit id, is answered as it was and applied nothing more,
// so the change is repeatable and a retried or duplicated change never applies
// a commit twice. Safety rests on this check and on ballots, never on locks
// held anywhere.
namespace kvasir::txn {

// A transaction's id: its coordinating node, that node's incarnation and its
// sequence there. Unique for the cluster's life.
struct CommitId {
  uint64_t node = 0;
  uint64_t incarnation = 0;
  uint64_t sequence = 0;
  auto operator<=>(const CommitId&) const = default;
};

// One row a transaction writes: the version it read the row at (0 for a row
// it found absent), and its new value (none: deleted).
struct RowWrite {
  std::string key;
  uint64_t expected = 0;
  std::optional<std::string> value;
};

// One transaction's commit: its id and its writes. (Not "Transaction":
// txn::Transaction is the store's, and one name for two types breaks the one
// definition rule across the library and its tests.)
struct Commit {
  CommitId id;
  std::vector<RowWrite> writes;
};

enum class Verdict : uint8_t { committed = 1, refused = 2 };

// A range's agreed state: its position (the number of transactions applied),
// each row with the position of the transaction that last wrote it, and the
// commit ids applied, each with its verdict.
struct RangeState {
  struct Row {
    uint64_t version = 0;
    std::string value;
    friend bool operator==(const Row&, const Row&) = default;
  };
  uint64_t position = 0;
  std::map<std::string, Row> rows;
  std::map<CommitId, Verdict> applied;
  friend bool operator==(const RangeState&, const RangeState&) = default;
};

// Applies the batch in order and gives each transaction's verdict. (Not
// "apply": a call with a std::vector would find std::apply too.)
std::vector<Verdict> apply_batch(RangeState& state, const std::vector<Commit>& batch);

// Dannan's wire encoding. A decode that cannot be completed throws
// dannan::net::DecodeError; empty bytes are a range never written.
dannan::net::Bytes encode_range(const RangeState& state);
RangeState decode_range(std::span<const uint8_t> bytes);
dannan::net::Bytes encode_verdicts(const std::vector<Verdict>& verdicts);
std::vector<Verdict> decode_verdicts(std::span<const uint8_t> bytes);

// The agreed change for one range's name: the batch applied to its current
// value, its result the batch's verdicts. Deterministic and repeatable.
dannan::agree::Change commit_change(std::vector<Commit> batch);

}  // namespace kvasir::txn
