//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// kvasir-node: one Kvasir node. It reads and checks its settings; it does not
// yet listen, since a MySQL client's TLS upgrade needs Dannan 0.8.4, and says
// so rather than listening on a port it cannot answer.
//
// Every setting is also a key in a YAML file (--config PATH, $KVASIR_CONFIG, or
// /usr/local/etc/kvasir/config.yaml when it exists), in secrets.yaml beside
// it, or a KVASIR_* variable, the flags winning.
//
//   kvasir-node --version
//   kvasir-node hash-password < password     a credential for the users setting
//   kvasir-node --ack-level replicated --tls-certificate node.pem --tls-key node.key --user "root:<credential>"

#include <iostream>
#include <string>
#include <vector>

#include "dannan/config/config.h"
#include "kvasir/server/accounts.h"
#include "kvasir/server/node_settings.h"
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
    std::cerr << "kvasir-node: " << kvasir::version << " read its settings (" << node.users.size() << " accounts, ack_level " << node.ack_level
              << ") but does not listen yet: a MySQL client's TLS upgrade needs Dannan 0.8.4" << std::endl;
    return 2;
  } catch (const dannan::config::ConfigError& e) {
    std::cerr << "kvasir-node: " << e.what() << std::endl;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "kvasir-node: " << e.what() << std::endl;
    return 1;
  }
}
