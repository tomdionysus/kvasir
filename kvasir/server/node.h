//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <string>

#include "dannan/listen/backoff.h"
#include "dannan/listen/listener.h"
#include "dannan/sched/worker_pool.h"
#include "kvasir/catalog/catalog.h"
#include "kvasir/exec/variables.h"
#include "kvasir/server/accounts.h"
#include "kvasir/server/node_settings.h"
#include "kvasir/server/pool_checker.h"
#include "kvasir/server/sign_in.h"
#include "kvasir/server/statistics.h"
#include "kvasir/txn/store.h"

// One node, in memory: what every connection refers to, the pool that checks
// passwords away from the connections, and the listener serving the MySQL
// protocol on settings.host:settings.port, clients signing in only after TLS.
// kvasir-node is this and a signal handler; the tests start one on a loopback
// port and drive it with a real MySQL client library.
namespace kvasir::server {

// What a client is told the server is.
struct NodeIdentity {
  // 8.4.<n>-kvasir, n the MySQL 8.4 release the differential gate was
  // recorded against (the design).
  std::string server_version = "8.4.13-kvasir";
  std::string version_comment = "Kvasir - The Distributed Relational Database (in memory, not durable)";
};

class Node {
 public:
  // Binds and starts accepting; nothing is served until run(). Throws
  // std::system_error when it cannot bind, std::runtime_error for TLS it
  // cannot set up or settings it cannot work with.
  Node(const NodeSettings& settings, NodeIdentity identity = {});
  // Stops the pool while the io context still lives (its answers are posted
  // there), then the listener and the io context go before what connections
  // refer to.
  ~Node();
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // Serves on `threads` threads, this one among them, until stop(). Blocks.
  void run(unsigned threads);
  // Stops accepting, closes the connections, and stops the io context; run()
  // returns. From any thread, the io context's own included.
  void stop();

  // The bound port: settings.port, or the one the system chose for 0.
  uint16_t port() const noexcept { return _listener.port(); }
  size_t accounts() const noexcept { return _accounts.size(); }
  boost::asio::io_context& io() noexcept { return _io; }

 private:
  const NodeIdentity _identity;
  const exec::VariableRegistry _registry;
  catalog::Catalog _catalog;
  txn::Store _rows;
  const Accounts _accounts;
  VerifierCache _verifiers;
  dannan::sched::ClassGates _gates;
  dannan::sched::WorkerPool _pool;
  PoolPasswordChecker _checker;
  dannan::listen::Backoff _principals;
  dannan::listen::Backoff _sources;
  const SignInBackoff _backoff;
  NodeStatistics _statistics;
  std::atomic<uint32_t> _next_id{1};
  // After everything above: connections the io context still holds refer to
  // it, so it goes first.
  boost::asio::io_context _io;
  dannan::listen::StreamListener _listener;
};

}  // namespace kvasir::server
