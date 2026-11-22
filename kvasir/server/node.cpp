//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/node.h"

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include "kvasir/server/connection.h"
#include "kvasir/server/stream_transport.h"

namespace kvasir::server {

namespace {

dannan::sched::WorkerPoolConfig pool_config() {
  dannan::sched::WorkerPoolConfig config;
  config.threads = 2;
  config.name = "kvasir-sign-in";
  config.reserve = {0, 0, 0, 0};
  return config;
}

dannan::listen::ListenerConfig listener_config(const NodeSettings& settings) {
  dannan::listen::ListenerConfig config;
  config.name = "mysql";
  config.host = settings.host;
  config.port = settings.port;
  config.certificates = std::make_shared<dannan::listen::Certificates>(dannan::listen::CertificatePaths{settings.certificate, settings.key, {}});
  // MySQL's TLS begins after the client's SSLRequest, in place.
  config.start_tls = true;
  return config;
}

}  // namespace

// Lugh's rules: five free failures per account, twenty per address, then a
// wait from a second, doubling, to fifteen minutes; forgotten after an hour
// without a failure.
Node::Node(const NodeSettings& settings, NodeIdentity identity)
    : _identity(std::move(identity)),
      _registry(settings.ack_level, _identity.server_version, _identity.version_comment),
      _accounts(settings.users),
      _pool(pool_config(), _gates),
      _checker(_accounts, _pool, dannan::contract::steady_time_source()),
      _principals({5, std::chrono::seconds(1), std::chrono::minutes(15), std::chrono::hours(1)}),
      _sources({20, std::chrono::seconds(1), std::chrono::minutes(15), std::chrono::hours(1)}),
      _backoff{_principals, _sources, dannan::contract::steady_time_source()},
      _statistics(dannan::contract::steady_time_source()),
      _listener(_io, listener_config(settings)) {
  _pool.start();
  _listener.start(_io.get_executor(), [this](std::shared_ptr<dannan::listen::Stream> stream) {
    ConnectionSettings connection;
    connection.server_version = _identity.server_version;
    connection.connection_id = _next_id++;
    const auto executor = stream->get_executor();
    std::make_shared<Connection>(std::make_shared<StreamTransport>(std::move(stream)), executor, connection, _registry, _catalog, _rows, _verifiers, _checker,
                                 &_backoff, &_statistics)
        ->start();
  });
}

Node::~Node() {
  _listener.stop();
  _io.stop();
  _pool.stop();
}

void Node::run(unsigned threads) {
  std::vector<std::thread> others;
  for (unsigned i = 1; i < threads; ++i) others.emplace_back([this] { _io.run(); });
  _io.run();
  for (auto& t : others) t.join();
}

void Node::stop() {
  _listener.stop();
  _io.stop();
}

}  // namespace kvasir::server
