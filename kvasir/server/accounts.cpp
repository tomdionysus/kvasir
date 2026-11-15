//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/accounts.h"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "dannan/identity/crypto.h"

namespace kvasir::server {

using dannan::identity::Argon2Costs;
using dannan::identity::Secret;

namespace {

inline constexpr std::string_view scheme = "argon2id";

// Splits on every `separator`.
std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const auto at = text.find(separator, start);
    parts.push_back(text.substr(start, at - start));
    if (at == std::string::npos) return parts;
    start = at + 1;
  }
}

// "<name>=<decimal>", the number at most a uint32_t.
uint32_t parameter(const std::string& text, char name) {
  if (text.size() < 3 || text[0] != name || text[1] != '=' || text.size() > 2 + 10)
    throw std::invalid_argument("a credential's costs are m=<KiB>,t=<passes>,p=<lanes>");
  uint64_t value = 0;
  for (size_t i = 2; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') throw std::invalid_argument("a credential's costs are decimal numbers");
    value = value * 10 + static_cast<uint64_t>(text[i] - '0');
  }
  if (value > UINT32_MAX) throw std::invalid_argument("a credential's cost is out of range");
  return static_cast<uint32_t>(value);
}

}  // namespace

std::string make_credential(std::span<const uint8_t> password, const Argon2Costs& costs) {
  if (!costs.valid()) throw std::invalid_argument("Argon2id's costs are not valid");
  const auto salt = dannan::identity::random_bytes(credential_salt_bytes);
  const auto tag = dannan::identity::argon2id(password, salt, costs, credential_tag_bytes);
  return std::string(scheme) + "$m=" + std::to_string(costs.memory_kib) + ",t=" + std::to_string(costs.iterations) + ",p=" + std::to_string(costs.lanes) + "$" +
         dannan::identity::hex(salt) + "$" + dannan::identity::hex(tag.reveal());
}

Credential parse_credential(const std::string& text) {
  const auto parts = split(text, '$');
  if (parts.size() != 4 || parts[0] != scheme) throw std::invalid_argument("a credential is argon2id$m=<KiB>,t=<passes>,p=<lanes>$<salt>$<tag>");
  const auto costs = split(parts[1], ',');
  if (costs.size() != 3) throw std::invalid_argument("a credential's costs are m=<KiB>,t=<passes>,p=<lanes>");
  Credential credential;
  credential.costs.memory_kib = parameter(costs[0], 'm');
  credential.costs.iterations = parameter(costs[1], 't');
  credential.costs.lanes = parameter(costs[2], 'p');
  if (!credential.costs.valid()) throw std::invalid_argument("a credential's costs are not ones Argon2id takes");

  auto salt = dannan::identity::unhex(parts[2]);
  if (!salt || salt->size() < credential_salt_bytes) throw std::invalid_argument("a credential's salt is at least 16 bytes in hex");
  credential.salt = std::move(*salt);
  auto tag = dannan::identity::unhex(parts[3]);
  if (!tag || tag->size() != credential_tag_bytes) {
    if (tag) dannan::identity::cleanse(*tag);
    throw std::invalid_argument("a credential's tag is 32 bytes in hex");
  }
  credential.tag = Secret(*tag);
  dannan::identity::cleanse(*tag);
  return credential;
}

Accounts::Accounts(const std::vector<std::string>& items) {
  for (size_t i = 0; i < items.size(); ++i) {
    const auto where = "users item " + std::to_string(i + 1);
    const auto colon = items[i].rfind(':');
    if (colon == std::string::npos) throw std::invalid_argument(where + " is not <name>:<credential>");
    std::string name = items[i].substr(0, colon);
    if (name.empty()) throw std::invalid_argument(where + " has an empty name");
    if (_accounts.count(name)) throw std::invalid_argument(where + " names an account an earlier item gave");
    try {
      _accounts.emplace(std::move(name), parse_credential(items[i].substr(colon + 1)));
    } catch (const std::invalid_argument& e) {
      throw std::invalid_argument(where + ": " + e.what());
    }
  }
  _absent.costs = _accounts.empty() ? Argon2Costs{} : parse_credential(items.front().substr(items.front().rfind(':') + 1)).costs;
  _absent.salt = dannan::identity::random_bytes(credential_salt_bytes);
  _absent.tag = Secret::random(credential_tag_bytes);
}

bool Accounts::check(const std::string& user, std::span<const uint8_t> password) const {
  const auto found = _accounts.find(user);
  const Credential& credential = found == _accounts.end() ? _absent : found->second;
  const auto tag = dannan::identity::argon2id(password, credential.salt, credential.costs, credential_tag_bytes);
  const bool matches = tag == credential.tag;
  return matches && found != _accounts.end();
}

}  // namespace kvasir::server
