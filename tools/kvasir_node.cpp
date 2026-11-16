//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// kvasir-node: one Kvasir node, in memory (step 2). It reads and checks its
// settings, then serves the MySQL protocol on listen.host:listen.port over
// Dannan's listener, clients signing in only after TLS. SIGINT or SIGTERM
// stops it.
//
// Every setting is also a key in a YAML file (--config PATH, $KVASIR_CONFIG, or
// /usr/local/etc/kvasir/config.yaml when it exists), in secrets.yaml beside
// it, or a KVASIR_* variable, the flags winning.
//
//   kvasir-node --version
//   kvasir-node hash-password < password     a credential for the users setting
//   kvasir-node --ack-level replicated --tls-certificate node.pem --tls-key node.key --user "root:<credential>"

#include <algorithm>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/listen/listener.h"
#include "dannan/sched/worker_pool.h"
#include "kvasir/catalog/catalog.h"
#include "kvasir/exec/variables.h"
#include "kvasir/server/accounts.h"
#include "kvasir/server/connection.h"
#include "kvasir/server/node_settings.h"
#include "kvasir/server/pool_checker.h"
#include "kvasir/server/stream_transport.h"
#include "kvasir/version.h"

namespace {

dannan::config::Reader reader() {
  dannan::config::Reader r;
  r.config_variable = "KVASIR_CONFIG";
  r.default_path = "/usr/local/etc/kvasir/config.yaml";
  r.settings = kvasir::server::node_settings();
  return r;
}

// One line from standard input, without its line end, as the password.
int hash_password() {
  std::string password;
  std::getline(std::cin, password);
  if (!password.empty() && password.back() == '\r') password.pop_back();
  if (password.empty()) {
    std::cerr << "kvasir-node: hash-password reads a password, which may not be empty, from standard input" << std::endl;
    return 2;
  }
  std::cout << kvasir::server::make_credential({reinterpret_cast<const uint8_t*>(password.data()), password.size()}) << std::endl;
  return 0;
}

// The version a client is told (the design: 8.4.<n>-kvasir, n the MySQL 8.4
// release the differential gate was recorded against).
constexpr const char* server_version = "8.4.13-kvasir";
constexpr const char* version_comment = "Kvasir - The Distributed Relational Database (in memory, not durable)";

int serve(const kvasir::server::NodeSettings& node) {
  // What every connection refers to, made before the io context and gone
  // after it.
  const kvasir::exec::VariableRegistry registry(node.ack_level, server_version, version_comment);
  kvasir::catalog::Catalog catalog;
  const kvasir::server::Accounts accounts(node.users);
  kvasir::server::VerifierCache verifiers;
  dannan::sched::ClassGates gates;
  dannan::sched::WorkerPoolConfig pool_config;
  pool_config.threads = 2;
  pool_config.name = "kvasir-sign-in";
  pool_config.reserve = {0, 0, 0, 0};
  dannan::sched::WorkerPool pool(pool_config, gates);
  pool.start();
  kvasir::server::PoolPasswordChecker checker(accounts, pool, dannan::contract::steady_time_source());

  boost::asio::io_context io;
  dannan::listen::ListenerConfig config;
  config.name = "mysql";
  config.host = node.host;
  config.port = node.port;
  config.certificates = std::make_shared<dannan::listen::Certificates>(dannan::listen::CertificatePaths{node.certificate, node.key, {}});
  // MySQL's TLS begins after the client's SSLRequest, in place.
  config.start_tls = true;
  dannan::listen::StreamListener listener(io, config);
  std::atomic<uint32_t> next_id{1};
  listener.start(io.get_executor(), [&](std::shared_ptr<dannan::listen::Stream> stream) {
    kvasir::server::ConnectionSettings settings;
    settings.server_version = server_version;
    settings.connection_id = next_id++;
    const auto executor = stream->get_executor();
    std::make_shared<kvasir::server::Connection>(std::make_shared<kvasir::server::StreamTransport>(std::move(stream)), executor, settings, registry, catalog,
                                                 verifiers, checker)
        ->start();
  });
  boost::asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code&, int) {
    listener.stop();
    io.stop();
  });
  std::cerr << "kvasir-node: " << kvasir::version << " serving MySQL on " << node.host << ":" << listener.port() << " (" << accounts.size()
            << " accounts, ack_level " << node.ack_level << ")" << std::endl;

  std::vector<std::thread> threads;
  const unsigned count = std::max(2u, std::thread::hardware_concurrency());
  for (unsigned i = 1; i < count; ++i) threads.emplace_back([&io] { io.run(); });
  io.run();
  for (auto& t : threads) t.join();
  // The pool posts its answers to the io context: it stops while that lives.
  pool.stop();
  std::cerr << "kvasir-node: stopped" << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto settings = reader().read(std::vector<std::string>(argv + 1, argv + argc));
    if (settings.toggle("version").value_or(false)) {
      std::cout << "kvasir-node " << kvasir::version << std::endl;
      return 0;
    }
    if (!settings.positional.empty()) {
      if (settings.positional.front() == "hash-password" && settings.positional.size() == 1) return hash_password();
      std::cerr << "kvasir-node: unknown command \"" << settings.positional.front() << "\"" << std::endl;
      return 2;
    }
    const auto node = kvasir::server::read_node_settings(settings);
    for (const auto& warning : settings.warnings) std::cerr << "kvasir-node: " << warning << std::endl;
    return serve(node);
  } catch (const dannan::config::ConfigError& e) {
    std::cerr << "kvasir-node: " << e.what() << std::endl;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "kvasir-node: " << e.what() << std::endl;
    return 1;
  }
}
