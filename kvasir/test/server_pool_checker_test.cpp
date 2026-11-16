//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <optional>
#include <string>
#include <thread>

#include "kvasir/server/pool_checker.h"

namespace kvasir::server {
namespace {

std::span<const uint8_t> bytes(const std::string& s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

struct Bed {
  // Cheap costs: the check's place, not its strength, is under test.
  const dannan::identity::Argon2Costs cheap{8, 1, 1};
  Accounts accounts{{"alice:" + make_credential(bytes("open sesame"), cheap)}};
  dannan::sched::ClassGates gates;
  dannan::sched::WorkerPool pool{dannan::sched::WorkerPoolConfig{2, "kvasir-test", {0, 0, 0, 0}, {16, 16, 16, 16}, {0, 0, 0, 0}}, gates};
  boost::asio::io_context io;
  bool stopped = false;
  Bed() { pool.start(); }
  ~Bed() { stop(); }
  void stop() {
    if (!stopped) pool.stop();
    stopped = true;
  }

  // The answer, and whether it came on the io context's thread.
  std::pair<std::optional<bool>, bool> check(PasswordChecker& checker, const std::string& user, const std::string& password) {
    std::optional<bool> answer;
    bool on_executor = false;
    const auto here = std::this_thread::get_id();
    auto guard = boost::asio::make_work_guard(io);
    checker.check(user, dannan::identity::Secret(bytes(password)), io.get_executor(), [&](bool ok) {
      answer = ok;
      on_executor = std::this_thread::get_id() == here;
      guard.reset();
    });
    io.restart();
    io.run_for(std::chrono::seconds(30));
    return {answer, on_executor};
  }
};

TEST(ServerPoolChecker, ChecksOnTheWorkersAndAnswersOnTheExecutor) {
  Bed bed;
  PoolPasswordChecker checker(bed.accounts, bed.pool, dannan::contract::steady_time_source());
  const auto right = bed.check(checker, "alice", "open sesame");
  EXPECT_EQ(right.first, true);
  EXPECT_TRUE(right.second);
  EXPECT_EQ(bed.check(checker, "alice", "open sesame!").first, false);
  EXPECT_EQ(bed.check(checker, "bob", "open sesame").first, false);
}

TEST(ServerPoolChecker, AStoppedPoolAnswersNo) {
  Bed bed;
  PoolPasswordChecker checker(bed.accounts, bed.pool, dannan::contract::steady_time_source());
  bed.stop();
  const auto answer = bed.check(checker, "alice", "open sesame");
  EXPECT_EQ(answer.first, false);
  EXPECT_TRUE(answer.second);
}

}  // namespace
}  // namespace kvasir::server
