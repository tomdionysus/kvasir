//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/pool_checker.h"

#include <atomic>
#include <boost/asio/post.hpp>
#include <memory>

namespace kvasir::server {

using dannan::sched::WorkerPool;

PoolPasswordChecker::PoolPasswordChecker(const Accounts& accounts, WorkerPool& pool, const dannan::contract::TimeSource& time,
                                         std::chrono::milliseconds deadline)
    : _accounts(accounts), _pool(pool), _time(time), _deadline(deadline) {}

void PoolPasswordChecker::check(std::string user, dannan::identity::Secret password, boost::asio::any_io_executor executor, std::function<void(bool)> done) {
  auto verdict = std::make_shared<std::atomic<bool>>(false);
  const dannan::sched::WorkContext context(dannan::sched::FrameType::foreground, _time.now() + _deadline, nullptr, "kvasir.sign_in");
  const auto submitted = _pool.submit(
      context,
      [accounts = &_accounts, user = std::move(user), password = std::move(password), verdict](WorkerPool::Turn&) {
        verdict->store(accounts->check(user, password.reveal()));
      },
      executor, [done, verdict](WorkerPool::Ended ended) { done(ended.how == WorkerPool::End::ran && verdict->load()); });
  if (!submitted.accepted) boost::asio::post(executor, [done] { done(false); });
}

}  // namespace kvasir::server
