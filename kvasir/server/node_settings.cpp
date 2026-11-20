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
      // A key a.b is the flag --a-b and the variable KVASIR_A_B (the
      // maintainer, 2026-11-20).
      Setting{"listen.host", Setting::Kind::value, "--listen-host", "KVASIR_LISTEN_HOST", "an address to listen on"},
      Setting{"listen.port", Setting::Kind::value, "--listen-port", "KVASIR_LISTEN_PORT", "a port, 0 to 65535"},
      Setting{"tls.certificate", Setting::Kind::value, "--tls-certificate", "KVASIR_TLS_CERTIFICATE", "a PEM certificate chain's path"},
      Setting{"tls.key", Setting::Kind::value, "--tls-key", "KVASIR_TLS_KEY", "a PEM private key's path"},
      Setting{"ack_level", Setting::Kind::value, "--ack-level", "KVASIR_ACK_LEVEL", "ephemeral, persisted, decided or replicated"},
      // No variable: the reader splits a list variable on commas, and a
      // credential has them (m=..,t=..,p=..).
      Setting{"users", Setting::Kind::list, "--users", "", "accounts, each \"<name>:<credential>\" (kvasir-node hash-password makes one)"},
      Setting{"log.level", Setting::Kind::value, "--log-level", "KVASIR_LOG_LEVEL", "the least a logged line is: debug, info, warn or error (info)"},
      Setting{"log.format", Setting::Kind::value, "--log-format", "KVASIR_LOG_FORMAT", "text, or json for a log shipper (text)"},
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

  const auto level = settings.choice("ack_level", exec::ack_level_names());
  if (!level) throw ConfigError("ack_level is required: ephemeral, persisted, decided or replicated; Kvasir has no built-in default");
  if (!exec::ack_level_kept(*dannan::contract::parse_ack_level(*level)))
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

  // Dannan's parsers, in any case; a value neither takes is refused by name.
  if (const auto log_level = settings.text("log.level")) {
    try {
      out.log_level = dannan::status::parse_log_level(*log_level);
    } catch (const std::invalid_argument&) {
      throw settings.refuse("log.level", "a log level: debug, info, warn or error");
    }
  }
  if (const auto format = settings.text("log.format")) {
    try {
      out.log_format = dannan::status::parse_log_format(*format);
    } catch (const std::invalid_argument&) {
      throw settings.refuse("log.format", "a log format: text or json");
    }
  }
  return out;
}

}  // namespace kvasir::server
