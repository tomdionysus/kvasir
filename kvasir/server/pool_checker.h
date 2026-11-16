//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>

#include "dannan/contract/time_source.h"
#include "dannan/sched/worker_pool.h"
#include "kvasir/server/accounts.h"
#include "kvasir/server/connection.h"

// The node's PasswordChecker: each full-path check (Argon2id, tens of
// milliseconds and megabytes) runs on Dannan's WorkerPool as foreground work,
// a person waiting, with a deadline; never on a connection's strand. The
// task owns the password's Secret. The answer is posted to the connection's
// executor: false for a wrong password, an unknown name, a check refused
// because the pool's queue is full, or one dropped past its deadline.
//
// Thread safety: check() from any thread; the accounts are read only. Waits:
// none in check(); the task waits on nothing but the CPU.
namespace kvasir::server {

class PoolPasswordChecker : public PasswordChecker {
 public:
  // `accounts`, `pool` and `time` outlive the checker.
  PoolPasswordChecker(const Accounts& accounts, dannan::sched::WorkerPool& pool, const dannan::contract::TimeSource& time,
                      std::chrono::milliseconds deadline = std::chrono::seconds(10));

  void check(std::string user, dannan::identity::Secret password, boost::asio::any_io_executor executor, std::function<void(bool)> done) override;

 private:
  const Accounts& _accounts;
  dannan::sched::WorkerPool& _pool;
  const dannan::contract::TimeSource& _time;
  const std::chrono::milliseconds _deadline;
};

}  // namespace kvasir::server
