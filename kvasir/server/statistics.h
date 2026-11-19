//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "dannan/contract/time_source.h"

// What a node answers to COM_STATISTICS, the `mysql` client's `status`
// line: how long the node has run, how many commands are running now, and
// how many questions it has had, in MySQL 8.4's words and spacing (observed
// on MySQL 8.4.13). Questions are a sign-in and every command but COM_PING
// and COM_STATISTICS (the manual, "Questions"). Kvasir has no table cache and
// no slow query log, so those fields are 0.
//
// One per node, shared by its connections' command phases.
//
// Thread safety: every operation is safe from any thread; each count is an
// atomic, and text() reads them one at a time, so a line read while commands
// run may mix moments. Waits: none.
namespace kvasir::server {

class NodeStatistics {
 public:
  // The node starts now, by `time`, which outlives this.
  explicit NodeStatistics(const dannan::contract::TimeSource& time);

  void question() noexcept;
  void command_began() noexcept;
  void command_ended() noexcept;

  // "Uptime: 540  Threads: 2  Questions: 2115  Slow queries: 0  Opens: 0
  // Flush tables: 0  Open tables: 0  Queries per second avg: 3.916"
  std::string text() const;

 private:
  const dannan::contract::TimeSource& _time;
  const dannan::contract::TimePoint _started;
  std::atomic<uint64_t> _questions{0};
  std::atomic<uint64_t> _running{0};
};

}  // namespace kvasir::server
