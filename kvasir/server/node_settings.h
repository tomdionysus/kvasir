//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/status/log.h"

// What a Kvasir node reads at start, through Dannan's settings reader (a YAML
// file, secrets.yaml beside it, KVASIR_* variables, then flags). A setting it
// cannot work with stops it at start, naming the setting (discipline 3: a
// configuration the node cannot work with is refused at startup).
namespace kvasir::server {

struct NodeSettings {
  std::string host = "0.0.0.0";
  uint16_t port = 3306;
  // Clients sign in only over TLS, so a node has both.
  std::string certificate;
  std::string key;
  // The default acknowledgement level; there is no built-in one (ruling 11).
  std::string ack_level;
  // "<name>:<credential>" items (server/accounts.h); at least one.
  std::vector<std::string> users;
  // The log's filter and form (the stack's log.level and log.format).
  dannan::status::LogLevel log_level = dannan::status::LogLevel::info;
  dannan::status::LogFormat log_format = dannan::status::LogFormat::text;
};

// The settings, for the reader and its usage text.
std::vector<dannan::config::Setting> node_settings();

// Reads and checks them. Throws dannan::config::ConfigError naming the
// setting and where it came from.
NodeSettings read_node_settings(const dannan::config::Settings& settings);

}  // namespace kvasir::server
