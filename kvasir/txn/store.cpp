//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/txn/store.h"

#include <algorithm>
#include <utility>

namespace kvasir::txn {

std::string Store::lock_name(uint64_t table, const std::string& name) { return std::to_string(table) + ":" + name; }

Transaction Store::begin() {
  dannan::contract::Lock lock(_mutex);
  Transaction t;
  t._id = _next_transaction++;
  return t;
}

void Store::take_snapshot(Transaction& t, bool fresh) {
  dannan::contract::Lock lock(_mutex);
  if (fresh || !t._snapshot) t._snapshot = _last_commit;
}

void Store::release(Transaction& t) {
  for (const auto& name : t._locks) {
    const auto it = _locks.find(name);
    if (it != _locks.end() && it->second == t._id) _locks.erase(it);
  }
  t._locks.clear();
  t._writes.clear();
  t._statement_writes.clear();
  t._statement_locks.clear();
  t._snapshot.reset();
}

std::vector<std::function<void()>> Store::waking(uint64_t holder) {
  std::vector<std::function<void()>> wakes;
  for (auto it = _waits.begin(); it != _waits.end();) {
    if (it->second.holder == holder) {
      wakes.push_back(std::move(it->second.wake));
      it = _waits.erase(it);
    } else {
      ++it;
    }
  }
  return wakes;
}

Store::Wait Store::wait(uint64_t waiter, uint64_t holder, std::function<void()> wake) {
  dannan::contract::Lock lock(_mutex);
  const bool holds = std::any_of(_locks.begin(), _locks.end(), [&](const auto& l) { return l.second == holder; });
  if (!holds) return Wait::free;
  // Follow what the holder waits for: back at the waiter is a cycle.
  for (uint64_t at = holder;;) {
    if (at == waiter) return Wait::deadlock;
    const auto next = _waits.find(at);
    if (next == _waits.end()) break;
    at = next->second.holder;
  }
  _waits[waiter] = {holder, std::move(wake)};
  return Wait::waiting;
}

void Store::stop_waiting(uint64_t waiter) {
  dannan::contract::Lock lock(_mutex);
  _waits.erase(waiter);
}

void Store::commit(Transaction& t) {
  std::vector<std::function<void()>> wakes;
  {
    dannan::contract::Lock lock(_mutex);
    commit_locked(t);
    wakes = waking(t._id);
    _waits.erase(t._id);
  }
  for (auto& wake : wakes) wake();
}

void Store::commit_locked(Transaction& t) {
  if (!t._writes.empty()) {
    const uint64_t at = ++_last_commit;
    for (auto& [table, writes] : t._writes) {
      const auto found = _tables.find(table);
      // A table dropped since takes its writes with it.
      if (found == _tables.end()) continue;
      auto& rows = found->second;
      for (auto& [key, pending] : writes) {
        rows.by_key[key].push_back({at, std::move(pending.row)});
        for (const auto& name : pending.unique) rows.unique[name] = key;
      }
    }
  }
  release(t);
}

void Store::rollback(Transaction& t) {
  std::vector<std::function<void()>> wakes;
  {
    dannan::contract::Lock lock(_mutex);
    release(t);
    wakes = waking(t._id);
    _waits.erase(t._id);
  }
  for (auto& wake : wakes) wake();
}

void Store::begin_statement(Transaction& t) {
  dannan::contract::Lock lock(_mutex);
  t._statement_writes.clear();
  t._statement_locks.clear();
}

void Store::undo_statement(Transaction& t) {
  std::vector<std::function<void()>> wakes;
  {
    dannan::contract::Lock lock(_mutex);
    for (const auto& [table, key] : t._statement_writes) {
      const auto found = t._writes.find(table);
      if (found == t._writes.end()) continue;
      found->second.erase(key);
      if (found->second.empty()) t._writes.erase(found);
    }
    for (const auto& name : t._statement_locks) {
      const auto it = _locks.find(name);
      if (it != _locks.end() && it->second == t._id) _locks.erase(it);
      t._locks.erase(name);
    }
    // Those waiting may find what they wanted free now.
    if (!t._statement_locks.empty()) wakes = waking(t._id);
    t._statement_writes.clear();
    t._statement_locks.clear();
  }
  for (auto& wake : wakes) wake();
}

Store::Inserted Store::insert(Transaction& t, uint64_t table, const std::string& key, const std::vector<std::string>& unique, Row row,
                              std::string* duplicate_of, uint64_t* holder) {
  dannan::contract::Lock lock(_mutex);
  auto& rows = _tables[table];
  // The names this row must hold: its key, and each unique value.
  std::vector<std::string> names{lock_name(table, "k" + key)};
  for (const auto& u : unique) names.push_back(lock_name(table, "u" + u));
  for (const auto& name : names) {
    const auto held = _locks.find(name);
    if (held != _locks.end() && held->second != t._id) {
      if (holder) *holder = held->second;
      return Inserted::locked;
    }
  }
  // A duplicate is judged against the latest committed rows, whatever the
  // snapshot (as InnoDB judges it), and against the transaction's own.
  const auto set_duplicate = [&](const std::string& what) {
    if (duplicate_of) *duplicate_of = what;
    return Inserted::duplicate;
  };
  if (rows.by_key.contains(key)) return set_duplicate("");
  if (const auto own = t._writes.find(table); own != t._writes.end()) {
    if (own->second.contains(key)) return set_duplicate("");
    for (const auto& [unused, pending] : own->second)
      for (const auto& u : unique)
        if (std::find(pending.unique.begin(), pending.unique.end(), u) != pending.unique.end()) return set_duplicate(u);
  }
  for (const auto& u : unique)
    if (rows.unique.contains(u)) return set_duplicate(u);
  for (const auto& name : names) {
    if (_locks.emplace(name, t._id).second) t._statement_locks.push_back(name);
    t._locks.insert(name);
  }
  t._writes[table][key] = {std::move(row), unique};
  t._statement_writes.emplace_back(table, key);
  return Inserted::inserted;
}

std::vector<Row> Store::rows(Transaction& t, uint64_t table) {
  dannan::contract::Lock lock(_mutex);
  if (!t._snapshot) t._snapshot = _last_commit;
  std::map<std::string, const Row*> seen;
  if (const auto found = _tables.find(table); found != _tables.end()) {
    for (const auto& [key, versions] : found->second.by_key) {
      // The newest version at or before the snapshot.
      for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        if (it->commit <= *t._snapshot) {
          seen[key] = &it->row;
          break;
        }
      }
    }
  }
  if (const auto own = t._writes.find(table); own != t._writes.end())
    for (const auto& [key, pending] : own->second) seen[key] = &pending.row;
  std::vector<Row> out;
  out.reserve(seen.size());
  for (const auto& [key, row] : seen) out.push_back(*row);
  return out;
}

uint64_t Store::reserve_auto(uint64_t table, uint64_t start, uint64_t count) {
  dannan::contract::Lock lock(_mutex);
  auto& rows = _tables[table];
  if (rows.next_auto < start) rows.next_auto = start;
  const uint64_t first = rows.next_auto;
  rows.next_auto += count;
  return first;
}

void Store::saw_auto(uint64_t table, uint64_t start, uint64_t value) {
  dannan::contract::Lock lock(_mutex);
  auto& rows = _tables[table];
  if (rows.next_auto < start) rows.next_auto = start;
  if (value >= rows.next_auto) rows.next_auto = value + 1;
}

uint64_t Store::next_row_id(uint64_t table) {
  dannan::contract::Lock lock(_mutex);
  return _tables[table].next_row_id++;
}

void Store::drop_table(uint64_t table) {
  dannan::contract::Lock lock(_mutex);
  _tables.erase(table);
}

}  // namespace kvasir::txn
