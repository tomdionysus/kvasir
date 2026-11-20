//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A node's settings: the design's rulings (ack_level has no built-in default
// and a node without it refuses to start, ruling 11; TLS before any
// authentication), MySQL's port 3306, and refusals that name the setting.

#include <gtest/gtest.h>

#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "kvasir/server/accounts.h"
#include "kvasir/server/node_settings.h"

namespace kvasir::server {
namespace {

class NoEnvironment : public dannan::config::Environment {
 public:
  std::optional<std::string> get(const std::string&) const override { return std::nullopt; }
};

dannan::config::Settings settings_of(std::vector<std::string> arguments) {
  static const NoEnvironment none;
  dannan::config::Reader reader;
  reader.settings = node_settings();
  reader.environment = &none;
  return reader.read(arguments);
}

std::string user() {
  dannan::identity::Argon2Costs cheap;
  cheap.memory_kib = 8;
  cheap.iterations = 1;
  cheap.lanes = 1;
  const std::string password = "secret";
  return "root:" + make_credential({reinterpret_cast<const uint8_t*>(password.data()), password.size()}, cheap);
}

std::vector<std::string> complete() { return {"--ack-level", "ephemeral", "--tls-certificate", "node.pem", "--tls-key", "node.key", "--users", user()}; }

std::string refusal(std::vector<std::string> arguments) {
  try {
    read_node_settings(settings_of(std::move(arguments)));
  } catch (const dannan::config::ConfigError& e) {
    return e.what();
  }
  return "";
}

TEST(ServerNodeSettings, ACompleteSetIsRead) {
  auto args = complete();
  args.insert(args.end(), {"--listen-host", "127.0.0.1", "--listen-port", "13306"});
  const auto s = read_node_settings(settings_of(args));
  EXPECT_EQ(s.host, "127.0.0.1");
  EXPECT_EQ(s.port, 13306);
  EXPECT_EQ(s.certificate, "node.pem");
  EXPECT_EQ(s.key, "node.key");
  EXPECT_EQ(s.ack_level, "ephemeral");
  ASSERT_EQ(s.users.size(), 1u);
}

TEST(ServerNodeSettings, MySqlsPortByDefault) {
  const auto s = read_node_settings(settings_of(complete()));
  EXPECT_EQ(s.host, "0.0.0.0");
  EXPECT_EQ(s.port, 3306);
}

TEST(ServerNodeSettings, AckLevelHasNoDefault) {
  auto args = complete();
  args.erase(args.begin(), args.begin() + 2);
  const auto message = refusal(args);
  EXPECT_NE(message.find("ack_level"), std::string::npos) << message;
  EXPECT_NE(message.find("no built-in default"), std::string::npos) << message;
}

TEST(ServerNodeSettings, AckLevelIsOneOfTheFour) {
  auto args = complete();
  args[1] = "durable";
  EXPECT_NE(refusal(args).find("ack_level"), std::string::npos);
}

// The log's level and format: the stack's names (log.level, log.format, agreed
// with Dannan 2026-11-20), read by Dannan's parsers in any case; INFO and text
// when not given; a value neither parser takes is refused, naming the setting.
TEST(ServerNodeSettings, LogLevelAndFormat) {
  using dannan::status::LogFormat;
  using dannan::status::LogLevel;
  auto s = read_node_settings(settings_of(complete()));
  EXPECT_EQ(s.log_level, LogLevel::info);
  EXPECT_EQ(s.log_format, LogFormat::text);
  auto args = complete();
  args.insert(args.end(), {"--log-level", "Warning", "--log-format", "JSON"});
  s = read_node_settings(settings_of(args));
  EXPECT_EQ(s.log_level, LogLevel::warn);
  EXPECT_EQ(s.log_format, LogFormat::json);
  args = complete();
  args.insert(args.end(), {"--log-level", "loud"});
  auto message = refusal(args);
  EXPECT_NE(message.find("log.level"), std::string::npos) << message;
  EXPECT_NE(message.find("loud"), std::string::npos) << message;
  args = complete();
  args.insert(args.end(), {"--log-format", "xml"});
  message = refusal(args);
  EXPECT_NE(message.find("log.format"), std::string::npos) << message;
  EXPECT_NE(message.find("xml"), std::string::npos) << message;
}

// This release is one node in memory: it keeps `ephemeral`'s promise (lost if
// the node stops) and no stronger one, so a node refuses to start at another
// (the maintainer's ruling for 0.1.0).
TEST(ServerNodeSettings, OnlyTheLevelsThisReleaseKeeps) {
  auto args = complete();
  args[1] = "ephemeral";
  EXPECT_EQ(read_node_settings(settings_of(args)).ack_level, "ephemeral");
  for (const char* level : {"persisted", "decided", "replicated"}) {
    args[1] = level;
    const auto message = refusal(args);
    EXPECT_NE(message.find(std::string("ack_level ") + level + " is not kept yet"), std::string::npos) << message;
    EXPECT_NE(message.find("only ephemeral"), std::string::npos) << message;
  }
}

TEST(ServerNodeSettings, TlsIsRequired) {
  auto args = complete();
  args.erase(args.begin() + 2, args.begin() + 4);
  EXPECT_NE(refusal(args).find("tls.certificate"), std::string::npos);
  args = complete();
  args.erase(args.begin() + 4, args.begin() + 6);
  EXPECT_NE(refusal(args).find("tls.key"), std::string::npos);
}

TEST(ServerNodeSettings, UsersAreRequiredAndChecked) {
  auto args = complete();
  args.erase(args.end() - 2, args.end());
  EXPECT_NE(refusal(args).find("users"), std::string::npos);
  args.insert(args.end(), {"--users", "root:not-a-credential"});
  const auto message = refusal(args);
  EXPECT_NE(message.find("users"), std::string::npos) << message;
  EXPECT_EQ(message.find("not-a-credential"), std::string::npos) << message;
}

// Each key a.b is the flag --a-b and the variable KVASIR_A_B (the maintainer,
// 2026-11-20); users has no variable, which would split its credentials.
TEST(ServerNodeSettings, NamesFollowTheKeys) {
  for (const auto& s : node_settings()) {
    if (s.key == "version") continue;
    std::string flag = "--", variable = "KVASIR_";
    for (const char c : s.key) {
      flag += (c == '.' || c == '_') ? '-' : c;
      variable += (c == '.') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    EXPECT_EQ(s.flag, flag) << s.key;
    EXPECT_EQ(s.variable, s.key == "users" ? "" : variable) << s.key;
  }
}

TEST(ServerNodeSettings, APortOutOfRangeIsRefused) {
  auto args = complete();
  args.insert(args.end(), {"--listen-port", "70000"});
  EXPECT_NE(refusal(args).find("listen.port"), std::string::npos);
}

}  // namespace
}  // namespace kvasir::server
