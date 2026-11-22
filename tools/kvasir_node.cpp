//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// kvasir-node: one Kvasir node, in memory. It reads and checks its settings,
// then serves the MySQL protocol on listen.host:listen.port over Dannan's
// listener, clients signing in only after TLS. SIGINT or SIGTERM stops it.
//
// Every setting is also a key in a YAML file (--config PATH, $KVASIR_CONFIG, or
// /etc/kvasir/config.yaml when it exists), in secrets.yaml beside it, or a
// KVASIR_* variable, the flags winning: a key a.b is --a-b and KVASIR_A_B
// (Dannan's docs/config.md).
//
//   kvasir-node --version
//   kvasir-node hash-password < password     a credential for the users setting
//   kvasir-node --ack-level ephemeral --tls-certificate node.pem --tls-key node.key --users "root:<credential>"

#include <algorithm>
#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/status/log.h"
#include "kvasir/server/accounts.h"
#include "kvasir/server/node.h"
#include "kvasir/server/node_settings.h"
#include "kvasir/version.h"

namespace {

dannan::config::Reader reader() {
  dannan::config::Reader r;
  r.config_variable = "KVASIR_CONFIG";
  r.default_path = "/etc/kvasir/config.yaml";
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

int serve(const kvasir::server::NodeSettings& settings, const dannan::status::ScopedLogger& log) {
  kvasir::server::Node node(settings);
  boost::asio::signal_set signals(node.io(), SIGINT, SIGTERM);
  signals.async_wait([&node](const boost::system::error_code&, int) { node.stop(); });
  log.info(std::string(kvasir::version) + " serving MySQL on " + settings.host + ":" + std::to_string(node.port()) + " (" + std::to_string(node.accounts()) +
           " accounts, ack_level " + settings.ack_level + ")");
  node.run(std::max(2u, std::thread::hardware_concurrency()));
  log.info("stopped");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // The process's sink from its first line, text at INFO on stderr; what is
  // behind it may be swapped later without the loggers handed out changing.
  const auto sink =
      std::make_shared<dannan::status::SwappableSink>(dannan::status::console_sink(dannan::status::LogFormat::text, dannan::status::LogLevel::info));
  const dannan::status::ScopedLogger log(sink, "kvasir-node");
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
    // From here, the node's own level and format.
    sink->set(dannan::status::console_sink(node.log_format, node.log_level));
    for (const auto& warning : settings.warnings) log.warn(warning);
    return serve(node, log);
  } catch (const dannan::config::ConfigError& e) {
    log.error(e.what());
    return 2;
  } catch (const std::exception& e) {
    log.error(e.what());
    return 1;
  }
}
