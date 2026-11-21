//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/txn/range_commit.h"

#include <utility>

namespace kvasir::txn {

namespace {

// The format's version, first: an unknown one is refused, never guessed.
constexpr uint8_t range_format = 1;

// The largest row value a range state holds (MySQL's largest row, a LONGBLOB).
constexpr size_t value_maximum = 4u * 1024 * 1024 * 1024 - 1;

uint64_t current_version(const RangeState& state, const std::string& key) {
  const auto it = state.rows.find(key);
  return it == state.rows.end() ? 0 : it->second.version;
}

}  // namespace

std::vector<Verdict> apply_batch(RangeState& state, const std::vector<Commit>& batch) {
  std::vector<Verdict> out;
  out.reserve(batch.size());
  for (const auto& transaction : batch) {
    if (const auto it = state.applied.find(transaction.id); it != state.applied.end()) {
      out.push_back(it->second);
      continue;
    }
    bool current = true;
    for (const auto& write : transaction.writes)
      if (current_version(state, write.key) != write.expected) current = false;
    if (!current) {
      state.applied.emplace(transaction.id, Verdict::refused);
      out.push_back(Verdict::refused);
      continue;
    }
    const uint64_t position = ++state.position;
    for (const auto& write : transaction.writes) {
      if (write.value)
        state.rows[write.key] = {position, *write.value};
      else
        state.rows.erase(write.key);
    }
    state.applied.emplace(transaction.id, Verdict::committed);
    out.push_back(Verdict::committed);
  }
  return out;
}

dannan::net::Bytes encode_range(const RangeState& state) {
  dannan::net::Writer w;
  w.u8(range_format);
  w.u64(state.position);
  w.u64(state.rows.size());
  for (const auto& [key, row] : state.rows) {
    w.string(key);
    w.u64(row.version);
    w.string(row.value);
  }
  w.u64(state.applied.size());
  for (const auto& [id, verdict] : state.applied) {
    w.u64(id.node);
    w.u64(id.incarnation);
    w.u64(id.sequence);
    w.u8(static_cast<uint8_t>(verdict));
  }
  return w.take();
}

namespace {

Verdict verdict_of(uint8_t byte) {
  if (byte != static_cast<uint8_t>(Verdict::committed) && byte != static_cast<uint8_t>(Verdict::refused))
    throw dannan::net::DecodeError("a commit verdict is 1 or 2, not " + std::to_string(byte));
  return static_cast<Verdict>(byte);
}

}  // namespace

RangeState decode_range(std::span<const uint8_t> bytes) {
  RangeState state;
  if (bytes.empty()) return state;
  dannan::net::Reader r(bytes);
  if (const auto format = r.u8(); format != range_format) throw dannan::net::DecodeError("a range's format is 1, not " + std::to_string(format));
  state.position = r.u64();
  // Each count is bounded by the bytes left, so a damaged count allocates
  // nothing it merely claims.
  const auto rows = r.u64();
  if (rows > r.remaining()) throw dannan::net::DecodeError("a range claims more rows than its bytes hold");
  for (uint64_t i = 0; i < rows; ++i) {
    auto key = r.string(value_maximum);
    RangeState::Row row;
    row.version = r.u64();
    row.value = r.string(value_maximum);
    state.rows.emplace(std::move(key), std::move(row));
  }
  const auto applied = r.u64();
  if (applied > r.remaining()) throw dannan::net::DecodeError("a range claims more commits than its bytes hold");
  for (uint64_t i = 0; i < applied; ++i) {
    CommitId id;
    id.node = r.u64();
    id.incarnation = r.u64();
    id.sequence = r.u64();
    state.applied.emplace(id, verdict_of(r.u8()));
  }
  r.finish();
  return state;
}

dannan::net::Bytes encode_verdicts(const std::vector<Verdict>& verdicts) {
  dannan::net::Writer w;
  w.u32(static_cast<uint32_t>(verdicts.size()));
  for (const auto verdict : verdicts) w.u8(static_cast<uint8_t>(verdict));
  return w.take();
}

std::vector<Verdict> decode_verdicts(std::span<const uint8_t> bytes) {
  dannan::net::Reader r(bytes);
  const auto count = r.u32();
  if (count > r.remaining()) throw dannan::net::DecodeError("a verdict list claims more verdicts than its bytes hold");
  std::vector<Verdict> out;
  for (uint32_t i = 0; i < count; ++i) out.push_back(verdict_of(r.u8()));
  r.finish();
  return out;
}

dannan::agree::Change commit_change(std::vector<Commit> batch) {
  return [batch = std::move(batch)](std::span<const std::optional<dannan::agree::Bytes>> current) {
    auto state = current.empty() || !current[0] ? RangeState{} : decode_range(*current[0]);
    const auto verdicts = apply_batch(state, batch);
    dannan::agree::Changed changed{{encode_range(state)}, encode_verdicts(verdicts)};
    changed.repeatable = true;
    return changed;
  };
}

}  // namespace kvasir::txn
