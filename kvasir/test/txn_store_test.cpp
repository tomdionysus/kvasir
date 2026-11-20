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

std::vector<std::string> current_values(Store& s, Transaction& t, uint64_t table) {
  std::vector<std::string> out;
  for (const auto& k : s.current(t, table)) out.push_back(k.key + "=" + k.row.at(0).value_or("NULL"));
  return out;
}

// UPDATE and DELETE: a row changed or gone, under its key or a new one; its
// unique values move with it; the transaction's own changes are its own until
// it commits, then everyone's.
TEST(TxnStore, UpdatesAndDeletes) {
  Store s;
  auto t = s.begin();
  s.insert(t, 1, "k1", {"u:1"}, row("x1"), nullptr);
  s.insert(t, 1, "k2", {"u:2"}, row("x2"), nullptr);
  s.commit(t);
  auto reader = s.begin();
  EXPECT_EQ(values(s, reader, 1), (std::vector<std::string>{"x1", "x2"}));

  auto w = s.begin();
  std::string which;
  EXPECT_EQ(s.update(w, 1, "k1", "k1", {"u:1"}, row("y1"), nullptr), Store::Inserted::inserted);
  EXPECT_EQ(current_values(s, w, 1), (std::vector<std::string>{"k1=y1", "k2=x2"}));
  EXPECT_EQ(values(s, w, 1), (std::vector<std::string>{"y1", "x2"}));
  // A unique value another row holds.
  EXPECT_EQ(s.update(w, 1, "k1", "k1", {"u:2"}, row("z"), &which), Store::Inserted::duplicate);
  EXPECT_EQ(which, "u:2");
  // Once that row goes, its value is free.
  EXPECT_TRUE(s.remove(w, 1, "k2"));
  EXPECT_EQ(s.update(w, 1, "k1", "k1", {"u:2"}, row("y1b"), nullptr), Store::Inserted::inserted);
  // A new primary key: the row moves.
  EXPECT_EQ(s.update(w, 1, "k1", "k3", {"u:2"}, row("y3"), nullptr), Store::Inserted::inserted);
  EXPECT_EQ(current_values(s, w, 1), (std::vector<std::string>{"k3=y3"}));

  // Another transaction meets the locks of rows changed, gone or moved.
  auto o = s.begin();
  uint64_t holder = 0;
  EXPECT_FALSE(s.remove(o, 1, "k1", &holder));
  EXPECT_EQ(holder, w.id());
  EXPECT_EQ(s.insert(o, 1, "k2", {}, row("o"), nullptr), Store::Inserted::locked);
  EXPECT_EQ(s.update(o, 1, "k3", "k3", {}, row("o"), nullptr), Store::Inserted::locked);
  s.rollback(o);

  // The reader keeps its snapshot through the commit.
  EXPECT_EQ(values(s, reader, 1), (std::vector<std::string>{"x1", "x2"}));
  s.commit(w);
  EXPECT_EQ(values(s, reader, 1), (std::vector<std::string>{"x1", "x2"}));
  auto later = s.begin();
  EXPECT_EQ(values(s, later, 1), (std::vector<std::string>{"y3"}));
  // The unique values moved with the row: 1 is free, 2 is taken.
  EXPECT_EQ(s.insert(later, 1, "k5", {"u:1"}, row("v5"), nullptr), Store::Inserted::inserted);
  EXPECT_EQ(s.insert(later, 1, "k6", {"u:2"}, row("v6"), &which), Store::Inserted::duplicate);
  EXPECT_EQ(which, "u:2");
  // A key whose row went may be used again.
  EXPECT_EQ(s.insert(later, 1, "k1", {}, row("again"), nullptr), Store::Inserted::inserted);
}

// An UPDATE builds on the latest commit, not on the transaction's snapshot
// (InnoDB's current read), so a concurrent change is not lost.
TEST(TxnStore, CurrentReadsTheLatestCommit) {
  Store s;
  auto t = s.begin();
  s.insert(t, 1, "k", {}, row("1"), nullptr);
  s.commit(t);
  auto reader = s.begin();
  EXPECT_EQ(values(s, reader, 1), (std::vector<std::string>{"1"}));
  auto other = s.begin();
  s.update(other, 1, "k", "k", {}, row("2"), nullptr);
  s.commit(other);
  EXPECT_EQ(values(s, reader, 1), (std::vector<std::string>{"1"}));
  EXPECT_EQ(current_values(s, reader, 1), (std::vector<std::string>{"k=2"}));
  // Rows deleted and committed are not current.
  auto d = s.begin();
  EXPECT_TRUE(s.remove(d, 1, "k"));
  s.commit(d);
  EXPECT_TRUE(current_values(s, reader, 1).empty());
  EXPECT_EQ(values(s, reader, 1), (std::vector<std::string>{"1"}));
}

TEST(TxnStore, AFailedStatementRestoresWhatItChanged) {
  Store s;
  auto t = s.begin();
  s.begin_statement(t);
  s.insert(t, 1, "k1", {"u:a"}, row("a"), nullptr);
  s.begin_statement(t);
  s.update(t, 1, "k1", "k1", {"u:b"}, row("b"), nullptr);
  s.update(t, 1, "k1", "k2", {"u:c"}, row("c"), nullptr);
  s.undo_statement(t);
  EXPECT_EQ(current_values(s, t, 1), (std::vector<std::string>{"k1=a"}));
  s.begin_statement(t);
  EXPECT_TRUE(s.remove(t, 1, "k1"));
  s.undo_statement(t);
  EXPECT_EQ(current_values(s, t, 1), (std::vector<std::string>{"k1=a"}));
  // The undone statement's locks are free; the first one's are kept.
  auto other = s.begin();
  EXPECT_EQ(s.insert(other, 1, "k2", {"u:c"}, row("o"), nullptr), Store::Inserted::inserted);
  EXPECT_EQ(s.insert(other, 1, "k9", {"u:a"}, row("o"), nullptr), Store::Inserted::locked);
}

// InnoDB's lock waits ("InnoDB Locking", "Deadlocks in InnoDB"): a writer
// meeting another's lock waits until that transaction ends; a wait that
// would close a cycle is a deadlock.
TEST(TxnStore, WaitsAndDeadlocks) {
  Store s;
  auto first = s.begin();
  auto second = s.begin();
  ASSERT_EQ(s.insert(first, 1, "a", {}, row("a"), nullptr), Store::Inserted::inserted);
  uint64_t holder = 0;
  EXPECT_EQ(s.insert(second, 1, "a", {}, row("a"), nullptr, &holder), Store::Inserted::locked);
  EXPECT_EQ(holder, first.id());
  int woken = 0;
  EXPECT_EQ(s.wait(second.id(), holder, [&] { ++woken; }), Store::Wait::waiting);
  EXPECT_EQ(woken, 0);
  s.commit(first);
  EXPECT_EQ(woken, 1);
  // Woken once only.
  auto third = s.begin();
  s.insert(third, 1, "b", {}, row("b"), nullptr);
  s.commit(third);
  EXPECT_EQ(woken, 1);
  EXPECT_EQ(s.insert(second, 1, "a", {}, row("a"), nullptr), Store::Inserted::duplicate);

  // A rollback wakes too, and so does a statement undone.
  auto x = s.begin();
  auto y = s.begin();
  s.begin_statement(x);
  s.insert(x, 1, "c", {}, row("c"), nullptr);
  EXPECT_EQ(s.insert(y, 1, "c", {}, row("c"), nullptr, &holder), Store::Inserted::locked);
  EXPECT_EQ(s.wait(y.id(), holder, [&] { ++woken; }), Store::Wait::waiting);
  s.undo_statement(x);
  EXPECT_EQ(woken, 2);
  EXPECT_EQ(s.insert(y, 1, "c", {}, row("c"), nullptr), Store::Inserted::inserted);

  // Each holds what the other wants: the second wait is a deadlock.
  auto p = s.begin();
  auto q = s.begin();
  s.insert(p, 2, "1", {}, row("1"), nullptr);
  s.insert(q, 2, "2", {}, row("2"), nullptr);
  EXPECT_EQ(s.insert(p, 2, "2", {}, row("2"), nullptr, &holder), Store::Inserted::locked);
  EXPECT_EQ(s.wait(p.id(), holder, [&] { ++woken; }), Store::Wait::waiting);
  EXPECT_EQ(s.insert(q, 2, "1", {}, row("1"), nullptr, &holder), Store::Inserted::locked);
  EXPECT_EQ(s.wait(q.id(), holder, [] {}), Store::Wait::deadlock);
  // The victim rolls back, and the other goes on.
  s.rollback(q);
  EXPECT_EQ(woken, 3);
  EXPECT_EQ(s.insert(p, 2, "2", {}, row("2"), nullptr), Store::Inserted::inserted);

  // A waiter that gives up is not woken; a holder that holds nothing now is
  // free.
  auto r = s.begin();
  auto w = s.begin();
  s.insert(r, 3, "k", {}, row("k"), nullptr);
  EXPECT_EQ(s.wait(w.id(), r.id(), [&] { ++woken; }), Store::Wait::waiting);
  s.stop_waiting(w.id());
  s.commit(r);
  EXPECT_EQ(woken, 3);
  EXPECT_EQ(s.wait(w.id(), r.id(), [&] { ++woken; }), Store::Wait::free);
  EXPECT_EQ(woken, 3);

  // A transaction's end wakes only those waiting on it.
  auto h1 = s.begin();
  auto h2 = s.begin();
  auto w1 = s.begin();
  auto w2 = s.begin();
  s.insert(h1, 4, "m", {}, row("m"), nullptr);
  s.insert(h2, 4, "n", {}, row("n"), nullptr);
  int woken1 = 0, woken2 = 0;
  EXPECT_EQ(s.wait(w1.id(), h1.id(), [&] { ++woken1; }), Store::Wait::waiting);
  EXPECT_EQ(s.wait(w2.id(), h2.id(), [&] { ++woken2; }), Store::Wait::waiting);
  s.commit(h1);
  EXPECT_EQ(woken1, 1);
  EXPECT_EQ(woken2, 0);
  s.commit(h2);
  EXPECT_EQ(woken2, 1);
}

}  // namespace
}  // namespace kvasir::txn
