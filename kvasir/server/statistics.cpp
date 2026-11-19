//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/statistics.h"

#include <chrono>

namespace kvasir::server {

NodeStatistics::NodeStatistics(const dannan::contract::TimeSource& time) : _time(time), _started(time.now()) {}

void NodeStatistics::question() noexcept { _questions.fetch_add(1, std::memory_order_relaxed); }
void NodeStatistics::command_began() noexcept { _running.fetch_add(1, std::memory_order_relaxed); }
void NodeStatistics::command_ended() noexcept { _running.fetch_sub(1, std::memory_order_relaxed); }

std::string NodeStatistics::text() const {
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(_time.now() - _started).count();
  const uint64_t uptime = elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
  const uint64_t questions = _questions.load(std::memory_order_relaxed);
  const uint64_t running = _running.load(std::memory_order_relaxed);
  // The average to three places, cut rather than rounded, as MySQL's.
  const uint64_t thousandths = uptime ? questions * 1000 / uptime : 0;
  std::string fraction = std::to_string(thousandths % 1000);
  fraction.insert(0, 3 - fraction.size(), '0');
  return "Uptime: " + std::to_string(uptime) + "  Threads: " + std::to_string(running) + "  Questions: " + std::to_string(questions) +
         "  Slow queries: 0  Opens: 0  Flush tables: 0  Open tables: 0  Queries per second avg: " + std::to_string(thousandths / 1000) + "." + fraction;
}

}  // namespace kvasir::server
