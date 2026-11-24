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
#include "dannan/identity/password.h"

// The accounts a node signs clients in against, until the Lugh plugin: built
// in, read from the `users` setting (secrets.yaml), each item
// "<name>:<credential>". A name is compared as bytes and may hold anything
// but is never empty; it ends at the item's last ':'. A credential is
// Dannan's stored password (rule 3: the stack's one copy) in its PHC form,
//
//   $argon2id$v=19$m=<KiB>,t=<passes>,p=<lanes>$<salt>$<hash>
//
// a 16-byte salt and a 32-byte hash in unpadded base64. Only the full path of
// caching_sha2_password checks a password here; the fast path's verifier is
// made from the password at that moment and kept in memory only
// (VerifierCache). A refusal names the item by its position, never its
// credential.
namespace kvasir::server {

using Credential = dannan::identity::PasswordHash;

// A new credential for `password` under a fresh random salt. Throws
// std::invalid_argument for costs that are not valid(). Spends the costs.
std::string make_credential(std::span<const uint8_t> password, const dannan::identity::Argon2Costs& costs = {});
// Throws std::invalid_argument for text that is not a credential Dannan's
// from_phc reads (another algorithm or version, costs out of bounds, a salt
// or hash of another length, anything after the hash).
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
  // The costs a name with no account spends (verify_decoy): the first
  // item's (the defaults when there are none), since accounts made together
  // share their costs.
  dannan::identity::Argon2Costs _absent;
};

}  // namespace kvasir::server
