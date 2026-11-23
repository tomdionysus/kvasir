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
  if (fresh || !t._snapshot) set_snapshot(t, _last_commit);
}

void Store::set_snapshot(Transaction& t, uint64_t at) {
  t._snapshot = at;
  _snapshots[t._id] = at;
}

std::vector<uint64_t> Store::live_snapshots() const {
  std::vector<uint64_t> live;
  for (const auto& [id, at] : _snapshots) live.push_back(at);
  return live;
}

bool Store::fold(uint64_t table, const std::string& key, const std::vector<uint64_t>& live) {
  const auto found = _tables.find(table);
  if (found == _tables.end()) return true;
  auto& by_key = found->second.by_key;
  const auto it = by_key.find(key);
  if (it == by_key.end()) return true;
  auto& versions = it->second;
  // The newest version, and the one each live snapshot sees; no snapshot
  // sees the rest.
  std::vector<bool> needed(versions.size(), false);
  needed.back() = true;
  for (const auto at : live) {
    for (size_t i = versions.size(); i-- > 0;)
      if (versions[i].commit <= at) {
        needed[i] = true;
        break;
      }
  }
  // Compacted in place; a version that stays where it is is not moved onto
  // itself.
  size_t kept = 0;
  for (size_t i = 0; i < versions.size(); ++i) {
    if (!needed[i]) continue;
    if (kept != i) versions[kept] = std::move(versions[i]);
    ++kept;
  }
  versions.resize(kept);
  if (versions.size() > 1) return false;
  // A deleted row every live snapshot sees deleted, or none saw, goes whole.
  if (!versions.front().row) by_key.erase(it);
  return true;
}

void Store::fold_written(const std::vector<std::pair<uint64_t, std::string>>& written) {
  const auto live = live_snapshots();
  for (const auto& [table, key] : written)
    if (!fold(table, key, live)) _unfolded.insert({table, key});
  for (auto it = _unfolded.begin(); it != _unfolded.end();) {
    if (fold(it->first, it->second, live)) {
      it = _unfolded.erase(it);
    } else {
      ++it;
    }
  }
}

size_t Store::versions(uint64_t table) const {
  dannan::contract::Lock lock(_mutex);
  size_t n = 0;
  if (const auto found = _tables.find(table); found != _tables.end())
    for (const auto& [key, versions] : found->second.by_key) n += versions.size();
  return n;
}

size_t Store::keys(uint64_t table) const {
  dannan::contract::Lock lock(_mutex);
  const auto found = _tables.find(table);
  return found == _tables.end() ? 0 : found->second.by_key.size();
}

bool Store::KeyRange::contains(const std::string& key) const {
  if (low && (low_inclusive ? key < *low : key <= *low)) return false;
  if (high && (high_inclusive ? key > *high : key >= *high)) return false;
  return true;
}

void Store::release(Transaction& t) {
  for (const auto& name : t._locks) {
    const auto it = _locks.find(name);
    if (it == _locks.end()) continue;
    if (it->second.exclusive == t._id) it->second.exclusive = 0;
    it->second.shared.erase(t._id);
    if (it->second.exclusive == 0 && it->second.shared.empty()) _locks.erase(it);
  }
  std::erase_if(_ranges, [&](const RangeLock& r) { return r.holder == t._id; });
  t._locks.clear();
  t._writes.clear();
  t._statement_undo.clear();
  t._statement_locks.clear();
  t._statement_ranges.clear();
  t._snapshot.reset();
  _snapshots.erase(t._id);
}

bool Store::holds_anything(uint64_t holder) const {
  for (const auto& [name, h] : _locks)
    if (h.exclusive == holder || h.shared.contains(holder)) return true;
  return std::any_of(_ranges.begin(), _ranges.end(), [&](const RangeLock& r) { return r.holder == holder; });
}

std::optional<uint64_t> Store::conflicting(const Transaction& t, const std::string& name, LockMode mode) const {
  const auto it = _locks.find(name);
  if (it == _locks.end()) return std::nullopt;
  if (it->second.exclusive != 0 && it->second.exclusive != t._id) return it->second.exclusive;
  if (mode == LockMode::exclusive)
    for (const auto other : it->second.shared)
      if (other != t._id) return other;
  return std::nullopt;
}

void Store::take(Transaction& t, const std::string& name, LockMode mode) {
  auto& h = _locks[name];
  const bool had_shared = h.shared.contains(t._id);
  if (h.exclusive == t._id || (mode == LockMode::shared && had_shared)) return;
  if (mode == LockMode::exclusive) {
    h.exclusive = t._id;
    h.shared.erase(t._id);
  } else {
    h.shared.insert(t._id);
  }
  t._statement_locks.emplace_back(name, had_shared);
  t._locks.insert(name);
}

std::optional<uint64_t> Store::range_holder(const Transaction& t, uint64_t table, const std::string& key) const {
  for (const auto& r : _ranges)
    if (r.table == table && r.holder != t._id && r.range.contains(key)) return r.holder;
  return std::nullopt;
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
  if (!holds_anything(holder)) return Wait::free;
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
  std::vector<std::pair<uint64_t, std::string>> written;
  if (!t._writes.empty()) {
    const uint64_t at = ++_last_commit;
    for (auto& [table, writes] : t._writes) {
      const auto found = _tables.find(table);
      // A table dropped since takes its writes with it.
      if (found == _tables.end()) continue;
      auto& rows = found->second;
      for (auto& [key, pending] : writes) {
        written.emplace_back(table, key);
        auto& versions = rows.by_key[key];
        // The unique values the row held go; those it holds now come.
        if (!versions.empty())
          for (const auto& name : versions.back().unique) {
            const auto held = rows.unique.find(name);
            if (held != rows.unique.end() && held->second == key) rows.unique.erase(held);
          }
        for (const auto& name : pending.unique) rows.unique[name] = key;
        versions.push_back({at, std::move(pending.row), std::move(pending.unique)});
      }
    }
  }
  release(t);
  // What the commit wrote, and what waited on its snapshot, folded.
  fold_written(written);
}

void Store::rollback(Transaction& t) {
  std::vector<std::function<void()>> wakes;
  {
    dannan::contract::Lock lock(_mutex);
    release(t);
    // Its snapshot gone, what waited on it may fold.
    fold_written({});
    wakes = waking(t._id);
    _waits.erase(t._id);
  }
  for (auto& wake : wakes) wake();
}

void Store::begin_statement(Transaction& t) {
  dannan::contract::Lock lock(_mutex);
  t._statement_undo.clear();
  t._statement_locks.clear();
  t._statement_ranges.clear();
}

void Store::undo_statement(Transaction& t) {
  std::vector<std::function<void()>> wakes;
  {
    dannan::contract::Lock lock(_mutex);
    for (auto undo = t._statement_undo.rbegin(); undo != t._statement_undo.rend(); ++undo) {
      auto& writes = t._writes[undo->table];
      if (undo->before) {
        writes[undo->key] = std::move(*undo->before);
      } else {
        writes.erase(undo->key);
      }
      if (writes.empty()) t._writes.erase(undo->table);
    }
    // Each lock as it was before the statement, newest first.
    for (auto taken = t._statement_locks.rbegin(); taken != t._statement_locks.rend(); ++taken) {
      const auto& [name, had_shared] = *taken;
      const auto it = _locks.find(name);
      if (it == _locks.end()) continue;
      auto& h = it->second;
      if (h.exclusive == t._id) {
        h.exclusive = 0;
        if (had_shared) h.shared.insert(t._id);
      } else {
        h.shared.erase(t._id);
      }
      const bool still = h.exclusive == t._id || h.shared.contains(t._id);
      if (h.exclusive == 0 && h.shared.empty()) _locks.erase(it);
      if (!still) t._locks.erase(name);
    }
    std::erase_if(_ranges,
                  [&](const RangeLock& r) { return std::find(t._statement_ranges.begin(), t._statement_ranges.end(), r.id) != t._statement_ranges.end(); });
    // Those waiting may find what they wanted free now.
    if (!t._statement_locks.empty() || !t._statement_ranges.empty()) wakes = waking(t._id);
    t._statement_undo.clear();
    t._statement_locks.clear();
    t._statement_ranges.clear();
  }
  for (auto& wake : wakes) wake();
}

bool Store::exists(const Transaction& t, uint64_t table, const Rows& rows, const std::string& key) const {
  if (const auto own = t._writes.find(table); own != t._writes.end())
    if (const auto p = own->second.find(key); p != own->second.end()) return p->second.row.has_value();
  const auto found = rows.by_key.find(key);
  return found != rows.by_key.end() && found->second.back().row.has_value();
}

std::optional<std::string> Store::unique_holder(const Transaction& t, uint64_t table, const Rows& rows, const std::string& name, const std::string& except,
                                                const std::string& except_too) const {
  const auto own = t._writes.find(table);
  if (own != t._writes.end())
    for (const auto& [key, pending] : own->second)
      if (key != except && key != except_too && pending.row && std::find(pending.unique.begin(), pending.unique.end(), name) != pending.unique.end())
        return key;
  const auto held = rows.unique.find(name);
  if (held == rows.unique.end() || held->second == except || held->second == except_too) return std::nullopt;
  // A row the transaction has itself changed holds what its change says.
  if (own != t._writes.end() && own->second.contains(held->second)) return std::nullopt;
  return held->second;
}

std::optional<uint64_t> Store::locked_by_other(const Transaction& t, const std::vector<std::string>& names) const {
  for (const auto& name : names)
    if (const auto other = conflicting(t, name, LockMode::exclusive)) return other;
  return std::nullopt;
}

void Store::take_locks(Transaction& t, const std::vector<std::string>& names) {
  for (const auto& name : names) take(t, name, LockMode::exclusive);
}

bool Store::lock_rows(Transaction& t, uint64_t table, const std::vector<std::string>& keys, LockMode mode, uint64_t* holder) {
  dannan::contract::Lock lock(_mutex);
  std::vector<std::string> names;
  for (const auto& key : keys) names.push_back(lock_name(table, "k" + key));
  for (const auto& name : names)
    if (const auto other = conflicting(t, name, mode)) {
      if (holder) *holder = *other;
      return false;
    }
  for (const auto& name : names) take(t, name, mode);
  return true;
}

bool Store::held_by_other(const Transaction& t, uint64_t table, const std::string& key, LockMode mode) {
  dannan::contract::Lock lock(_mutex);
  return conflicting(t, lock_name(table, "k" + key), mode).has_value();
}

bool Store::lock_range(Transaction& t, uint64_t table, const KeyRange& range, uint64_t* holder) {
  dannan::contract::Lock lock(_mutex);
  // Another's write in the range not yet committed holds its row's lock
  // exclusively; the names of one table's rows sort as their keys.
  const std::string prefix = lock_name(table, "k");
  for (auto it = _locks.lower_bound(prefix + range.low.value_or("")); it != _locks.end() && it->first.starts_with(prefix); ++it) {
    const std::string key = it->first.substr(prefix.size());
    if (!range.contains(key)) {
      if (range.high && key > *range.high) break;
      continue;
    }
    if (it->second.exclusive != 0 && it->second.exclusive != t._id) {
      if (holder) *holder = it->second.exclusive;
      return false;
    }
  }
  const uint64_t id = _next_range++;
  _ranges.push_back({id, t._id, table, range});
  t._statement_ranges.push_back(id);
  return true;
}

void Store::write(Transaction& t, uint64_t table, const std::string& key, Transaction::Pending pending) {
  auto& writes = t._writes[table];
  std::optional<Transaction::Pending> before;
  if (const auto p = writes.find(key); p != writes.end()) before = p->second;
  t._statement_undo.push_back({table, key, std::move(before)});
  writes[key] = std::move(pending);
}

Store::Inserted Store::insert(Transaction& t, uint64_t table, const std::string& key, const std::vector<std::string>& unique, Row row,
                              std::string* duplicate_of, uint64_t* holder) {
  dannan::contract::Lock lock(_mutex);
  auto& rows = _tables[table];
  // The names this row must hold: its key, and each unique value.
  std::vector<std::string> names{lock_name(table, "k" + key)};
  for (const auto& u : unique) names.push_back(lock_name(table, "u" + u));
  // Another's lock on the row's names, or a range lock over its key.
  auto other = locked_by_other(t, names);
  if (!other) other = range_holder(t, table, key);
  if (other) {
    if (holder) *holder = *other;
    return Inserted::locked;
  }
  // A duplicate is judged against the latest committed rows, whatever the
  // snapshot (as InnoDB judges it), and against the transaction's own.
  const auto set_duplicate = [&](const std::string& what) {
    if (duplicate_of) *duplicate_of = what;
    return Inserted::duplicate;
  };
  if (exists(t, table, rows, key)) return set_duplicate("");
  for (const auto& u : unique)
    if (unique_holder(t, table, rows, u, key, key)) return set_duplicate(u);
  take_locks(t, names);
  write(t, table, key, {std::move(row), unique});
  return Inserted::inserted;
}

Store::Inserted Store::update(Transaction& t, uint64_t table, const std::string& key, const std::string& new_key, const std::vector<std::string>& unique,
                              Row row, std::string* duplicate_of, uint64_t* holder) {
  dannan::contract::Lock lock(_mutex);
  auto& rows = _tables[table];
  std::vector<std::string> names{lock_name(table, "k" + key)};
  if (new_key != key) names.push_back(lock_name(table, "k" + new_key));
  for (const auto& u : unique) names.push_back(lock_name(table, "u" + u));
  // A key moved into another's locked range waits as an insert there does.
  auto other = locked_by_other(t, names);
  if (!other && new_key != key) other = range_holder(t, table, new_key);
  if (other) {
    if (holder) *holder = *other;
    return Inserted::locked;
  }
  const auto set_duplicate = [&](const std::string& what) {
    if (duplicate_of) *duplicate_of = what;
    return Inserted::duplicate;
  };
  if (new_key != key && exists(t, table, rows, new_key)) return set_duplicate("");
  for (const auto& u : unique)
    if (unique_holder(t, table, rows, u, key, new_key)) return set_duplicate(u);
  take_locks(t, names);
  if (new_key != key) write(t, table, key, {std::nullopt, {}});
  write(t, table, new_key, {std::move(row), unique});
  return Inserted::inserted;
}

bool Store::remove(Transaction& t, uint64_t table, const std::string& key, uint64_t* holder) {
  dannan::contract::Lock lock(_mutex);
  _tables[table];
  const std::vector<std::string> names{lock_name(table, "k" + key)};
  if (const auto other = locked_by_other(t, names)) {
    if (holder) *holder = *other;
    return false;
  }
  take_locks(t, names);
  write(t, table, key, {std::nullopt, {}});
  return true;
}

std::vector<Row> Store::rows(Transaction& t, uint64_t table) {
  dannan::contract::Lock lock(_mutex);
  if (!t._snapshot) set_snapshot(t, _last_commit);
  std::map<std::string, const std::optional<Row>*> seen;
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
  for (const auto& [key, row] : seen)
    if (*row) out.push_back(**row);
  return out;
}

std::vector<Store::Keyed> Store::current(Transaction& t, uint64_t table) {
  dannan::contract::Lock lock(_mutex);
  std::map<std::string, const std::optional<Row>*> seen;
  if (const auto found = _tables.find(table); found != _tables.end())
    for (const auto& [key, versions] : found->second.by_key) seen[key] = &versions.back().row;
  if (const auto own = t._writes.find(table); own != t._writes.end())
    for (const auto& [key, pending] : own->second) seen[key] = &pending.row;
  std::vector<Keyed> out;
  for (const auto& [key, row] : seen)
    if (*row) out.push_back({key, **row});
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
  std::erase_if(_unfolded, [&](const auto& waiting) { return waiting.first == table; });
}

}  // namespace kvasir::txn
