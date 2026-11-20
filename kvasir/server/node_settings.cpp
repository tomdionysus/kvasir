//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/node_settings.h"

#include <algorithm>
#include <stdexcept>

#include "kvasir/exec/variables.h"
#include "kvasir/server/accounts.h"

namespace kvasir::server {

using dannan::config::ConfigError;
using dannan::config::Setting;

std::vector<Setting> node_settings() {
  return {
      Setting{"version", Setting::Kind::toggle, "--version", "", "print the version and exit"},
      Setting{"listen.host", Setting::Kind::value, "--listen", "KVASIR_LISTEN", "an address to listen on"},
      Setting{"listen.port", Setting::Kind::value, "--port", "KVASIR_PORT", "a port, 0 to 65535"},
      Setting{"tls.certificate", Setting::Kind::value, "--tls-certificate", "KVASIR_TLS_CERTIFICATE", "a PEM certificate chain's path"},
      Setting{"tls.key", Setting::Kind::value, "--tls-key", "KVASIR_TLS_KEY", "a PEM private key's path"},
      Setting{"ack_level", Setting::Kind::value, "--ack-level", "KVASIR_ACK_LEVEL", "ephemeral, persisted, decided or replicated"},
      Setting{"users", Setting::Kind::list, "--user", "", "accounts, each \"<name>:<credential>\" (kvasir-node hash-password makes one)"},
  };
}

NodeSettings read_node_settings(const dannan::config::Settings& settings) {
  NodeSettings out;
  if (const auto host = settings.text("listen.host")) out.host = *host;
  if (const auto port = settings.number("listen.port", 0, 65535)) out.port = static_cast<uint16_t>(*port);

  const auto certificate = settings.path_of("tls.certificate");
  if (!certificate) throw ConfigError("tls.certificate is required: clients sign in only over TLS");
  const auto key = settings.path_of("tls.key");
  if (!key) throw ConfigError("tls.key is required: clients sign in only over TLS");
  out.certificate = *certificate;
  out.key = *key;

  const auto level = settings.choice("ack_level", exec::ack_levels);
  if (!level) throw ConfigError("ack_level is required: ephemeral, persisted, decided or replicated; Kvasir has no built-in default");
  if (std::find(exec::kept_ack_levels.begin(), exec::kept_ack_levels.end(), *level) == exec::kept_ack_levels.end())
    throw ConfigError("ack_level " + *level + " is not kept yet: this release is one node in memory, so only ephemeral");
  out.ack_level = *level;

  out.users = settings.list("users");
  if (out.users.empty()) throw ConfigError("users is required: at least one \"<name>:<credential>\" (kvasir-node hash-password makes a credential)");
  try {
    Accounts check(out.users);
  } catch (const std::invalid_argument& e) {
    const auto* value = settings.find("users");
    throw ConfigError((value ? value->source + ": " : std::string()) + e.what());
  }
  return out;
}

}  // namespace kvasir::server
