//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A commit on one range (TODO/2026-11-20-commit-protocol.md, "A commit on one
// range"): each transaction of a batch is checked against its rows' expected
// versions inside the agreed change, applied whole or refused whole, and
// applied at most once by its commit id.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "dannan/net/codec.h"
#include "kvasir/txn/range_commit.h"

namespace kvasir::txn {
namespace {

Commit txn(uint64_t sequence, std::vector<RowWrite> writes) { return {CommitId{1, 1, sequence}, std::move(writes)}; }

TEST(TxnRangeCommit, AWriteFromTheCurrentVersionCommits) {
  RangeState state;
  EXPECT_EQ(apply_batch(state, {txn(1, {{"a", 0, "x"}, {"b", 0, "y"}})}), (std::vector<Verdict>{Verdict::committed}));
  EXPECT_EQ(state.position, 1u);
  EXPECT_EQ(state.rows.at("a").version, 1u);
  EXPECT_EQ(state.rows.at("a").value, "x");
  EXPECT_EQ(state.rows.at("b").version, 1u);
  EXPECT_EQ(apply_batch(state, {txn(2, {{"a", 1, "x2"}})}), (std::vector<Verdict>{Verdict::committed}));
  EXPECT_EQ(state.rows.at("a").version, 2u);
  EXPECT_EQ(state.rows.at("a").value, "x2");
  EXPECT_EQ(state.rows.at("b").version, 1u);
}

// The one rule: of two writers of one row from one version, the second is
// refused, whole: none of its other writes is applied.
TEST(TxnRangeCommit, AStaleWriteIsRefusedWhole) {
  RangeState state;
  apply_batch(state, {txn(1, {{"a", 0, "x"}})});
  EXPECT_EQ(apply_batch(state, {txn(2, {{"a", 1, "first"}}), txn(3, {{"b", 0, "y"}, {"a", 1, "second"}})}),
            (std::vector<Verdict>{Verdict::committed, Verdict::refused}));
  EXPECT_EQ(state.rows.at("a").value, "first");
  EXPECT_FALSE(state.rows.contains("b"));
  EXPECT_EQ(state.position, 2u);
}

// A row expected absent (version 0) is refused once it exists, and a deleted
// row's later reinsertion takes a new version, so an expectation of the old
// one is refused (no ABA).
TEST(TxnRangeCommit, AbsenceAndDeletionAreVersioned) {
  RangeState state;
  apply_batch(state, {txn(1, {{"a", 0, "x"}})});
  EXPECT_EQ(apply_batch(state, {txn(2, {{"a", 0, "again"}})}), (std::vector<Verdict>{Verdict::refused}));
  EXPECT_EQ(apply_batch(state, {txn(3, {{"a", 1, std::nullopt}})}), (std::vector<Verdict>{Verdict::committed}));
  EXPECT_FALSE(state.rows.contains("a"));
  EXPECT_EQ(apply_batch(state, {txn(4, {{"a", 0, "back"}})}), (std::vector<Verdict>{Verdict::committed}));
  EXPECT_EQ(state.rows.at("a").version, 3u);
  EXPECT_EQ(apply_batch(state, {txn(5, {{"a", 1, "stale"}})}), (std::vector<Verdict>{Verdict::refused}));
}

// A batch applied again (a retried change) applies nothing twice and answers
// each transaction as it was answered: what makes the change repeatable.
TEST(TxnRangeCommit, ABatchAppliedTwiceAppliesOnce) {
  RangeState state;
  const std::vector<Commit> batch{txn(1, {{"a", 0, "x"}}), txn(2, {{"a", 0, "y"}})};
  EXPECT_EQ(apply_batch(state, batch), (std::vector<Verdict>{Verdict::committed, Verdict::refused}));
  const auto after = state;
  EXPECT_EQ(apply_batch(state, batch), (std::vector<Verdict>{Verdict::committed, Verdict::refused}));
  EXPECT_EQ(state, after);
}

TEST(TxnRangeCommit, StateAndVerdictsRoundTrip) {
  RangeState state;
  apply_batch(state, {txn(1, {{"a", 0, "x"}, {"b", 0, std::string("\0y", 2)}}), txn(2, {{"a", 1, std::nullopt}})});
  EXPECT_EQ(decode_range(encode_range(state)), state);
  EXPECT_EQ(decode_range({}), RangeState{});
  const std::vector<Verdict> verdicts{Verdict::committed, Verdict::refused};
  EXPECT_EQ(decode_verdicts(encode_verdicts(verdicts)), verdicts);
  // Bytes cut short, or a verdict that is not one, are refused, never guessed.
  auto bytes = encode_range(state);
  bytes.pop_back();
  EXPECT_THROW(decode_range(bytes), dannan::net::DecodeError);
  EXPECT_THROW(decode_verdicts(dannan::net::Bytes{0, 0, 0, 1, 7}), dannan::net::DecodeError);
}

// The agreed change: given the range's current value (none, before its first
// change), the new value and the batch's verdicts, and it is repeatable.
TEST(TxnRangeCommit, TheChangeAppliesTheBatch) {
  const auto change = commit_change({txn(1, {{"a", 0, "x"}}), txn(2, {{"a", 0, "y"}})});
  const std::vector<std::optional<dannan::agree::Bytes>> none{std::nullopt};
  const auto changed = change(none);
  ASSERT_EQ(changed.values.size(), 1u);
  ASSERT_TRUE(changed.values[0]);
  EXPECT_TRUE(changed.repeatable);
  const auto state = decode_range(*changed.values[0]);
  EXPECT_EQ(state.rows.at("a").value, "x");
  EXPECT_EQ(decode_verdicts(changed.result), (std::vector<Verdict>{Verdict::committed, Verdict::refused}));
  // Run again on its own output (a retry that finds it applied): the same.
  const std::vector<std::optional<dannan::agree::Bytes>> current{changed.values[0]};
  const auto again = change(current);
  EXPECT_EQ(again.values, changed.values);
  EXPECT_EQ(again.result, changed.result);
}

}  // namespace
}  // namespace kvasir::txn
