//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"

// A node's rows, in memory (step 2a): every table's rows as versions, each
// stamped with the commit that wrote it, and the transactions that read and
// write them. Step 2b replaces it with each range's state in Dagda, in the
// cluster's shape whatever the node count (the design's "One truth" and
// "Transactions"); the shape of a transaction is the design's: writes buffered at the
// coordinator, row locks taken as they are written, a snapshot per
// transaction (REPEATABLE READ) or per statement (READ COMMITTED), and every
// write of a transaction made visible at once by its commit.
//
// Rows are keyed by bytes that sort as the table's primary key does (the
// caller encodes them; a table with no primary key keys rows by a hidden row
// id). A unique key is a lock name too, so two transactions inserting one
// value contend for it. The store knows nothing of SQL types.
//
// Waiting: a writer that meets another transaction's lock is told who holds
// it, and may wait for that transaction to let its locks go (InnoDB's lock
// wait); the store refuses a wait that closes a cycle (a deadlock). The
// deadline is the caller's.
//
// Thread safety: every operation takes the store's one lock, held only for
// the operation; safe from any thread. No operation waits on anything else.
namespace kvasir::txn {

// Each column's value as stored, in the text a client is sent; nothing for
// NULL.
using Row = std::vector<std::optional<std::string>>;

class Store;

// One transaction's state. Belongs to one session; the store reads and
// changes it only under its own lock, through the calls below.
class Transaction {
 public:
  uint64_t id() const noexcept { return _id; }
  bool has_writes() const noexcept { return !_writes.empty(); }

 private:
  friend class Store;
  uint64_t _id = 0;
  // A row as this transaction has written it: nothing when deleted.
  struct Pending {
    std::optional<Row> row;
    std::vector<std::string> unique;
  };
  struct Undo {
    uint64_t table;
    std::string key;
    // What the transaction had written there before the statement, if
    // anything.
    std::optional<Pending> before;
  };
  // The commit sequence it reads at, once it has read.
  std::optional<uint64_t> _snapshot;
  // Rows it wrote, by table and key, not yet committed.
  std::map<uint64_t, std::map<std::string, Pending>> _writes;
  // Lock names it holds.
  std::set<std::string> _locks;
  // What the statement under way changed, undone in reverse if it fails.
  std::vector<Undo> _statement_undo;
  // Lock names the statement took, each with whether the transaction held it
  // shared before (a statement that took it exclusively gives back only
  // that).
  std::vector<std::pair<std::string, bool>> _statement_locks;
  // Range locks the statement took, by their ids.
  std::vector<uint64_t> _statement_ranges;
};

class Store {
 public:
  // A new transaction with no snapshot yet.
  Transaction begin();
  // Takes the snapshot now, if it has none (the first read of a REPEATABLE
  // READ transaction, every statement of a READ COMMITTED one).
  void take_snapshot(Transaction& t, bool fresh);
  // Makes its writes visible, as one commit, and frees its locks.
  void commit(Transaction& t);
  // Drops its writes and frees its locks.
  void rollback(Transaction& t);
  // Marks the start of a statement: what it adds can be undone alone.
  void begin_statement(Transaction& t);
  void undo_statement(Transaction& t);

  enum class Inserted : uint8_t { inserted, duplicate, locked };
  // Inserts a row under `key`, locking it and each name in `unique` (the
  // row's non-NULL unique key values, encoded with their key's name).
  // `duplicate_of` is set to the unique name, or "" for the primary key,
  // that another row already holds; `holder` to the transaction holding a
  // lock it needs.
  Inserted insert(Transaction& t, uint64_t table, const std::string& key, const std::vector<std::string>& unique, Row row, std::string* duplicate_of,
                  uint64_t* holder = nullptr);

  // A transaction (by id) waits for another to release its locks: `wake` is
  // called once, on the thread that releases them (commit, rollback, or a
  // statement undone), and must only post. A wait that would close a cycle
  // of waits is a deadlock and is not made; a holder that holds nothing now
  // is free, and nothing is waited for.
  enum class Wait : uint8_t { waiting, deadlock, free };
  Wait wait(uint64_t waiter, uint64_t holder, std::function<void()> wake);
  // The waiter gives up (its timeout, its session's end); `wake` is dropped.
  void stop_waiting(uint64_t waiter);

  // UPDATE: the row under `key` becomes `row` under `new_key` (the same key
  // unless its primary key changed), holding the unique names given. Locks
  // both keys and the names; a duplicate is judged as for insert, the row's
  // own old values not counting.
  Inserted update(Transaction& t, uint64_t table, const std::string& key, const std::string& new_key, const std::vector<std::string>& unique, Row row,
                  std::string* duplicate_of, uint64_t* holder = nullptr);
  // DELETE: the row under `key` goes. False, with `holder` set, when another
  // transaction holds its lock.
  bool remove(Transaction& t, uint64_t table, const std::string& key, uint64_t* holder = nullptr);

  // Locking reads (SELECT ... FOR SHARE and FOR UPDATE, INSERT ... SELECT's
  // source). A row's lock is held by one transaction exclusively (every
  // write takes it so) or by any number in shared mode; one alone in shared
  // mode may take it exclusively. Held until the transaction ends, or given
  // back with an undone statement.
  enum class LockMode : uint8_t { shared, exclusive };
  // The rows under `keys` locked for `t` in `mode`, all or none: false, with
  // `holder` set to one of them, when another transaction holds a lock that
  // conflicts.
  bool lock_rows(Transaction& t, uint64_t table, const std::vector<std::string>& keys, LockMode mode, uint64_t* holder = nullptr);
  // Whether another transaction holds the row's lock in a way `mode` would
  // conflict with (SKIP LOCKED passes over such rows).
  bool held_by_other(const Transaction& t, uint64_t table, const std::string& key, LockMode mode);
  // Keys from `low` to `high`, each bound inclusive or not; no bound for
  // nothing (every key, by default).
  struct KeyRange {
    std::optional<std::string> low;
    bool low_inclusive = true;
    std::optional<std::string> high;
    bool high_inclusive = true;
    bool contains(const std::string& key) const;
  };
  // The range locked for `t` (InnoDB's next-key locks): another transaction's
  // insert of a key in it, or an update that moves a key into it, is locked
  // out until `t` ends. Readers share a range. False, with `holder` set, when
  // another transaction has written a key in it not yet committed.
  bool lock_range(Transaction& t, uint64_t table, const KeyRange& range, uint64_t* holder = nullptr);

  // The rows `t` sees, in key order: its snapshot's, and its own writes.
  std::vector<Row> rows(Transaction& t, uint64_t table);
  // What UPDATE and DELETE read (InnoDB's current read): the latest
  // committed rows and the transaction's own writes, whatever its
  // snapshot, each with its key.
  struct Keyed {
    std::string key;
    Row row;
  };
  std::vector<Keyed> current(Transaction& t, uint64_t table);

  // AUTO_INCREMENT: `count` values from the table's counter, the first
  // returned; it starts at `start` the first time.
  uint64_t reserve_auto(uint64_t table, uint64_t start, uint64_t count);
  // A value given explicitly: the counter moves past it.
  void saw_auto(uint64_t table, uint64_t start, uint64_t value);
  // The hidden row id of a table with no primary key.
  uint64_t next_row_id(uint64_t table);

  // A table is gone: its rows, versions and counters with it.
  void drop_table(uint64_t table);

 private:
  // A committed row as of a commit: nothing when deleted then.
  struct Version {
    uint64_t commit = 0;
    std::optional<Row> row;
    std::vector<std::string> unique;
  };
  struct Rows {
    std::map<std::string, std::vector<Version>> by_key;
    // Committed unique names, to the key of the row that holds each.
    std::map<std::string, std::string> unique;
    uint64_t next_auto = 0;
    uint64_t next_row_id = 1;
  };
  struct Waiting {
    uint64_t holder = 0;
    std::function<void()> wake;
  };
  // A lock name's holders: one exclusive, or any number shared.
  struct Holders {
    uint64_t exclusive = 0;
    std::set<uint64_t> shared;
  };
  struct RangeLock {
    uint64_t id = 0;
    uint64_t holder = 0;
    uint64_t table = 0;
    KeyRange range;
  };
  static std::string lock_name(uint64_t table, const std::string& name);
  // Another transaction holding a range lock over `key`, if any.
  std::optional<uint64_t> range_holder(const Transaction& t, uint64_t table, const std::string& key) const DANNAN_REQUIRES(_mutex);
  // Whether `holder` holds any lock or range now.
  bool holds_anything(uint64_t holder) const DANNAN_REQUIRES(_mutex);
  // The names locked for `t` in `mode`, recording each for the statement's
  // undo.
  void take(Transaction& t, const std::string& name, LockMode mode) DANNAN_REQUIRES(_mutex);
  // A transaction other than `t` whose hold on `name` conflicts with `mode`.
  std::optional<uint64_t> conflicting(const Transaction& t, const std::string& name, LockMode mode) const DANNAN_REQUIRES(_mutex);
  // Whether a row is under `key` now, as `t` sees it for writing.
  bool exists(const Transaction& t, uint64_t table, const Rows& rows, const std::string& key) const DANNAN_REQUIRES(_mutex);
  // The key of another row holding a unique name now, as `t` sees it for
  // writing, leaving out `except`.
  std::optional<std::string> unique_holder(const Transaction& t, uint64_t table, const Rows& rows, const std::string& name, const std::string& except,
                                           const std::string& except_too) const DANNAN_REQUIRES(_mutex);
  // The lock names' holder other than `t`, if any.
  std::optional<uint64_t> locked_by_other(const Transaction& t, const std::vector<std::string>& names) const DANNAN_REQUIRES(_mutex);
  void take_locks(Transaction& t, const std::vector<std::string>& names) DANNAN_REQUIRES(_mutex);
  // Sets what `t` has written under a key, keeping what was there for the
  // statement's undo.
  void write(Transaction& t, uint64_t table, const std::string& key, Transaction::Pending pending) DANNAN_REQUIRES(_mutex);
  void release(Transaction& t) DANNAN_REQUIRES(_mutex);
  void commit_locked(Transaction& t) DANNAN_REQUIRES(_mutex);
  // The wakes of those waiting for `holder`, taken out to be called once the
  // lock is let go.
  std::vector<std::function<void()>> waking(uint64_t holder) DANNAN_REQUIRES(_mutex);

  mutable dannan::contract::Mutex _mutex;
  // Waiter to what it waits for.
  std::map<uint64_t, Waiting> _waits DANNAN_GUARDED_BY(_mutex);
  std::map<uint64_t, Rows> _tables DANNAN_GUARDED_BY(_mutex);
  // Lock name to the transactions holding it.
  std::map<std::string, Holders> _locks DANNAN_GUARDED_BY(_mutex);
  std::vector<RangeLock> _ranges DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_range DANNAN_GUARDED_BY(_mutex) = 1;
  uint64_t _last_commit DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _next_transaction DANNAN_GUARDED_BY(_mutex) = 1;
};

}  // namespace kvasir::txn
