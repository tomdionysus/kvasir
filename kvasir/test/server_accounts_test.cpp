//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The built-in accounts (the design, "Compatibility and authentication": the
// user list built in, from YAML, until the Lugh plugin; names compared as
// bytes; nothing of authentication data in a refusal). The credential's
// format is Kvasir's, stated in accounts.h. Costs here are Argon2id's least,
// so the tests are quick; the defaults are RFC 9106's.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "kvasir/server/accounts.h"

namespace kvasir::server {
namespace {

std::span<const uint8_t> as_bytes(std::string_view text) { return {reinterpret_cast<const uint8_t*>(text.data()), text.size()}; }

dannan::identity::Argon2Costs cheap() {
  dannan::identity::Argon2Costs costs;
  costs.memory_kib = 8;
  costs.iterations = 1;
  costs.lanes = 1;
  return costs;
}

std::string item(const std::string& name, const std::string& password) { return name + ":" + make_credential(as_bytes(password), cheap()); }

// The message of the std::invalid_argument `make` throws, or "" if none.
template <typename F>
std::string refusal(F make) {
  try {
    make();
  } catch (const std::invalid_argument& e) {
    return e.what();
  }
  return "";
}

TEST(ServerAccounts, ACredentialStatesItsCostsAndParsesBack) {
  const auto text = make_credential(as_bytes("secret"), cheap());
  EXPECT_EQ(text.rfind("argon2id$m=8,t=1,p=1$", 0), 0u) << text;
  const auto credential = parse_credential(text);
  EXPECT_EQ(credential.costs, cheap());
  EXPECT_EQ(credential.salt.size(), credential_salt_bytes);
  EXPECT_EQ(credential.tag.size(), credential_tag_bytes);
}

TEST(ServerAccounts, EachCredentialHasItsOwnSalt) { EXPECT_NE(make_credential(as_bytes("secret"), cheap()), make_credential(as_bytes("secret"), cheap())); }

TEST(ServerAccounts, TheRightPasswordMatches) {
  const Accounts accounts({item("root", "secret"), item("app", "other")});
  EXPECT_EQ(accounts.size(), 2u);
  EXPECT_TRUE(accounts.check("root", as_bytes("secret")));
  EXPECT_TRUE(accounts.check("app", as_bytes("other")));
}

TEST(ServerAccounts, AWrongPasswordDoesNot) {
  const Accounts accounts({item("root", "secret")});
  EXPECT_FALSE(accounts.check("root", as_bytes("Secret")));
  EXPECT_FALSE(accounts.check("root", as_bytes("")));
  EXPECT_FALSE(accounts.check("root", as_bytes("secret ")));
}

TEST(ServerAccounts, AnUnknownNameNeverMatches) {
  const Accounts accounts({item("root", "secret")});
  EXPECT_FALSE(accounts.check("nobody", as_bytes("secret")));
  EXPECT_FALSE(accounts.contains("nobody"));
  const Accounts none({});
  EXPECT_FALSE(none.check("root", as_bytes("secret")));
}

TEST(ServerAccounts, NamesAreComparedAsBytes) {
  const Accounts accounts({item("root", "secret")});
  EXPECT_FALSE(accounts.check("Root", as_bytes("secret")));
  EXPECT_FALSE(accounts.check("root ", as_bytes("secret")));
}

TEST(ServerAccounts, ANameEndsAtTheLastColon) {
  const Accounts accounts({item("svc:reports", "secret")});
  EXPECT_TRUE(accounts.contains("svc:reports"));
  EXPECT_TRUE(accounts.check("svc:reports", as_bytes("secret")));
}

TEST(ServerAccounts, BadItemsAreRefusedByPositionAndNeverQuoted) {
  const auto good = item("root", "secret");
  const auto credential = good.substr(good.rfind(':') + 1);

  auto message = refusal([&] { Accounts({good, "no colon here"}); });
  EXPECT_NE(message.find("2"), std::string::npos) << message;
  EXPECT_EQ(message.find("no colon here"), std::string::npos) << message;

  message = refusal([&] { Accounts({":" + credential}); });
  EXPECT_FALSE(message.empty());
  EXPECT_EQ(message.find(credential), std::string::npos) << message;

  message = refusal([&] { Accounts({good, good}); });
  EXPECT_FALSE(message.empty());
  EXPECT_EQ(message.find(credential), std::string::npos) << message;

  message = refusal([&] { Accounts({"root:argon2id$m=8,t=1,p=1$zz$00"}); });
  EXPECT_FALSE(message.empty());
  EXPECT_EQ(message.find("zz"), std::string::npos) << message;
}

TEST(ServerAccounts, MalformedCredentialsAreRefused) {
  const auto good = make_credential(as_bytes("secret"), cheap());
  const auto salt_at = good.find('$', good.find('$') + 1) + 1;
  const auto tag_at = good.rfind('$') + 1;
  const auto salt = good.substr(salt_at, tag_at - 1 - salt_at);
  const auto tag = good.substr(tag_at);

  EXPECT_THROW(parse_credential(""), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2i$m=8,t=1,p=1$" + salt + "$" + tag), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=8,t=1$" + salt + "$" + tag), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$t=1,m=8,p=1$" + salt + "$" + tag), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=8,t=1,p=1$" + salt + "$" + tag + "$"), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=8,t=1,p=1$" + salt), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=x,t=1,p=1$" + salt + "$" + tag), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=99999999999999999999,t=1,p=1$" + salt + "$" + tag), std::invalid_argument);
  // Costs Argon2 refuses: no passes, and past Dannan's ceiling.
  EXPECT_THROW(parse_credential("argon2id$m=8,t=0,p=1$" + salt + "$" + tag), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=8,t=65,p=1$" + salt + "$" + tag), std::invalid_argument);
  // A short salt, a tag of the wrong length, and text that is not hex.
  EXPECT_THROW(parse_credential("argon2id$m=8,t=1,p=1$" + salt.substr(2) + "$" + tag), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=8,t=1,p=1$" + salt + "$" + tag.substr(2)), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=8,t=1,p=1$" + salt + "$" + tag + "00"), std::invalid_argument);
  EXPECT_THROW(parse_credential("argon2id$m=8,t=1,p=1$" + salt + "$" + tag.substr(1) + "g"), std::invalid_argument);
}

TEST(ServerAccounts, MakingACredentialRefusesInvalidCosts) {
  auto costs = cheap();
  costs.lanes = 0;
  EXPECT_THROW(make_credential(as_bytes("secret"), costs), std::invalid_argument);
}

}  // namespace
}  // namespace kvasir::server
