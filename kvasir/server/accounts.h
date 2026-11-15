//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "dannan/identity/argon2.h"
#include "dannan/identity/secure_memory.h"

// The accounts a node signs clients in against, until the Lugh plugin: built
// in, read from the `users` setting (secrets.yaml), each item
// "<name>:<credential>". A name is compared as bytes and may hold anything
// but is never empty; it ends at the item's last ':'. A credential is
//
//   argon2id$m=<KiB>,t=<passes>,p=<lanes>$<salt, hex>$<tag, hex>
//
// Argon2id through Dannan's primitive, a 32-byte tag over a salt of at least
// 16 bytes. Only the full path of caching_sha2_password checks a password
// here; the fast path's verifier is made from the password at that moment and
// kept in memory only (VerifierCache). A refusal names the item by its
// position, never its credential.
namespace kvasir::server {

struct Credential {
  dannan::identity::Argon2Costs costs;
  std::vector<uint8_t> salt;
  dannan::identity::Secret tag;
};

inline constexpr size_t credential_salt_bytes = 16;
inline constexpr size_t credential_tag_bytes = 32;

// A new credential for `password` under a fresh random salt. Throws
// std::invalid_argument for costs that are not valid(). Spends the costs.
std::string make_credential(std::span<const uint8_t> password, const dannan::identity::Argon2Costs& costs = {});
// Throws std::invalid_argument for text that is not a credential, a salt
// shorter than credential_salt_bytes, a tag that is not credential_tag_bytes,
// or costs that are not valid().
Credential parse_credential(const std::string& text);

class Accounts {
 public:
  // Throws std::invalid_argument for an item with no ':', an empty name, a
  // name given twice, or a credential parse_credential refuses.
  explicit Accounts(const std::vector<std::string>& items);

  // Whether `password` is the account's. An unknown name costs the same
  // work as a known one, against a credential no password matches, so the
  // time taken does not say which names exist. Spends the account's costs:
  // run on a worker, never a connection's strand. Thread-safe: reads only.
  bool check(const std::string& user, std::span<const uint8_t> password) const;

  bool contains(const std::string& user) const { return _accounts.count(user) != 0; }
  size_t size() const noexcept { return _accounts.size(); }

 private:
  std::map<std::string, Credential> _accounts;
  // Checked against for a name with no account: a random tag under the
  // first item's costs (the defaults when there are none), since accounts
  // made together share their costs.
  Credential _absent;
};

}  // namespace kvasir::server
