//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The node's rows and transactions. The MySQL 8.4 manual, "Consistent
// Nonlocking Reads": a REPEATABLE READ transaction reads the snapshot its
// first read took, and sees its own changes; READ COMMITTED takes a fresh
// one each statement. "Locks Set by Different SQL Statements in InnoDB": an
// insert locks its row, and a duplicate key is judged against the latest
// committed row. The design: a commit makes all of a transaction's writes
// visible at once; a failed statement undoes only itself.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kvasir/txn/store.h"

namespace kvasir::txn {
namespace {

Row row(const std::string& v) { return Row{v}; }

std::vector<std::string> values(Store& s, Transaction& t, uint64_t table) {
  std::vector<std::string> out;
  for (const auto& r : s.rows(t, table)) out.push_back(r.at(0).value_or("NULL"));
  return out;
}

TEST(TxnStore, ACommitIsSeenByLaterTransactionsOnly) {
  Store s;
  auto writer = s.begin();
  auto reader = s.begin();
  EXPECT_EQ(s.insert(writer, 1, "b", {}, row("b"), nullptr), Store::Inserted::inserted);
  EXPECT_EQ(s.insert(writer, 1, "a", {}, row("a"), nullptr), Store::Inserted::inserted);
  // Its own writes, in key order; nobody else's.
  EXPECT_EQ(values(s, writer, 1), (std::vector<std::string>{"a", "b"}));
  EXPECT_TRUE(values(s, reader, 1).empty());
  s.commit(writer);
  // The reader's snapshot was taken at its first read: still nothing.
  EXPECT_TRUE(values(s, reader, 1).empty());
  // READ COMMITTED: a fresh snapshot each statement.
  s.take_snapshot(reader, true);
  EXPECT_EQ(values(s, reader, 1), (std::vector<std::string>{"a", "b"}));
  auto later = s.begin();
  EXPECT_EQ(values(s, later, 1), (std::vector<std::string>{"a", "b"}));
}

TEST(TxnStore, RollbackLeavesNothing) {
  Store s;
  auto t = s.begin();
  s.insert(t, 1, "a", {}, row("a"), nullptr);
  EXPECT_TRUE(t.has_writes());
  s.rollback(t);
  EXPECT_FALSE(t.has_writes());
  auto u = s.begin();
  EXPECT_TRUE(values(s, u, 1).empty());
  // Its lock is gone too.
  EXPECT_EQ(s.insert(u, 1, "a", {}, row("a"), nullptr), Store::Inserted::inserted);
}

TEST(TxnStore, DuplicatesAndLocks) {
  Store s;
  auto t = s.begin();
  std::string which = "unset";
  EXPECT_EQ(s.insert(t, 1, "k1", {"e:1"}, row("1"), nullptr), Store::Inserted::inserted);
  EXPECT_EQ(s.insert(t, 1, "k1", {}, row("x"), &which), Store::Inserted::duplicate);
  EXPECT_EQ(which, "");
  EXPECT_EQ(s.insert(t, 1, "k2", {"e:1"}, row("2"), &which), Store::Inserted::duplicate);
  EXPECT_EQ(which, "e:1");
  // Another transaction meets the locks of rows not yet committed.
  auto other = s.begin();
  EXPECT_EQ(s.insert(other, 1, "k1", {}, row("y"), nullptr), Store::Inserted::locked);
  EXPECT_EQ(s.insert(other, 1, "k3", {"e:1"}, row("y"), nullptr), Store::Inserted::locked);
  // Another table's keys are its own.
  EXPECT_EQ(s.insert(other, 2, "k1", {"e:1"}, row("z"), nullptr), Store::Inserted::inserted);
  s.commit(t);
  // Committed: a duplicate whatever the snapshot.
  EXPECT_EQ(s.insert(other, 1, "k1", {}, row("y"), &which), Store::Inserted::duplicate);
  EXPECT_EQ(which, "");
  EXPECT_EQ(s.insert(other, 1, "k3", {"e:1"}, row("y"), &which), Store::Inserted::duplicate);
  EXPECT_EQ(which, "e:1");
  EXPECT_EQ(s.insert(other, 1, "k3", {"e:2"}, row("3"), nullptr), Store::Inserted::inserted);
}

TEST(TxnStore, AFailedStatementUndoesOnlyItself) {
  Store s;
  auto t = s.begin();
  s.begin_statement(t);
  s.insert(t, 1, "a", {}, row("a"), nullptr);
  s.begin_statement(t);
  s.insert(t, 1, "b", {"u:b"}, row("b"), nullptr);
  s.undo_statement(t);
  EXPECT_EQ(values(s, t, 1), (std::vector<std::string>{"a"}));
  // The undone row's locks are free.
  auto other = s.begin();
  EXPECT_EQ(s.insert(other, 1, "b", {"u:b"}, row("b"), nullptr), Store::Inserted::inserted);
  EXPECT_EQ(s.insert(other, 1, "a", {}, row("a"), nullptr), Store::Inserted::locked);
}

TEST(TxnStore, CountersAndDrops) {
  Store s;
  EXPECT_EQ(s.reserve_auto(1, 1, 1), 1u);
  EXPECT_EQ(s.reserve_auto(1, 1, 2), 2u);
  EXPECT_EQ(s.reserve_auto(1, 1, 1), 4u);
  s.saw_auto(1, 1, 10);
  EXPECT_EQ(s.reserve_auto(1, 1, 1), 11u);
  s.saw_auto(1, 1, 3);
  EXPECT_EQ(s.reserve_auto(1, 1, 1), 12u);
  // AUTO_INCREMENT = 5 as a table option.
  EXPECT_EQ(s.reserve_auto(2, 5, 1), 5u);
  EXPECT_EQ(s.next_row_id(1), 1u);
  EXPECT_EQ(s.next_row_id(1), 2u);
  auto t = s.begin();
  s.insert(t, 1, "a", {}, row("a"), nullptr);
  s.commit(t);
  s.drop_table(1);
  auto u = s.begin();
  EXPECT_TRUE(values(s, u, 1).empty());
  EXPECT_EQ(s.reserve_auto(1, 1, 1), 1u);
}

}  // namespace
}  // namespace kvasir::txn
