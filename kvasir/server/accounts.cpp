//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/accounts.h"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace kvasir::server {

using dannan::identity::Argon2Costs;

std::string make_credential(std::span<const uint8_t> password, const Argon2Costs& costs) {
  return dannan::identity::to_phc(dannan::identity::make_password(password, costs));
}

Credential parse_credential(const std::string& text) {
  auto stored = dannan::identity::from_phc(text);
  if (!stored) throw std::invalid_argument("a credential is $argon2id$v=19$m=<KiB>,t=<passes>,p=<lanes>$<salt>$<hash>");
  return *stored;
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
  _absent = _accounts.empty() ? Argon2Costs{} : parse_credential(items.front().substr(items.front().rfind(':') + 1)).costs;
}

bool Accounts::check(const std::string& user, std::span<const uint8_t> password) const {
  const auto found = _accounts.find(user);
  if (found == _accounts.end()) return dannan::identity::verify_decoy(password, _absent);
  return dannan::identity::verify_password(password, found->second);
}

}  // namespace kvasir::server
