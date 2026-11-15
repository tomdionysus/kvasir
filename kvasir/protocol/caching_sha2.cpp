//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/protocol/caching_sha2.h"

#include <algorithm>
#include <array>

#include "dannan/identity/crypto.h"

namespace kvasir::protocol::caching_sha2 {

using dannan::identity::Hash256;
using dannan::identity::Secret;

namespace {

// SHA256(stage2 || nonce): the mask the scramble's first stage is hidden under.
Hash256 mask(std::span<const uint8_t> stage2, std::span<const uint8_t, nonce_size> nonce) {
  dannan::identity::Sha256Hasher hasher;
  hasher.update(stage2);
  hasher.update(nonce);
  return hasher.finish();
}

}  // namespace

Bytes scramble(std::span<const uint8_t> password, std::span<const uint8_t, nonce_size> nonce) {
  if (password.empty()) return {};
  auto stage1 = dannan::identity::sha256(password);
  const auto stage2 = dannan::identity::sha256(stage1.bytes);
  const auto masked = mask(stage2.bytes, nonce);
  Bytes out(scramble_size);
  for (size_t i = 0; i < scramble_size; ++i) out[i] = stage1.bytes[i] ^ masked.bytes[i];
  dannan::identity::cleanse(stage1.bytes);
  return out;
}

Secret fast_verifier(std::span<const uint8_t> password) {
  auto stage1 = dannan::identity::sha256(password);
  auto stage2 = dannan::identity::sha256(stage1.bytes);
  Secret verifier(stage2.bytes);
  dannan::identity::cleanse(stage1.bytes);
  dannan::identity::cleanse(stage2.bytes);
  return verifier;
}

bool check_scramble(std::span<const uint8_t> scramble, std::span<const uint8_t, nonce_size> nonce, const Secret& verifier) {
  if (scramble.size() != scramble_size || verifier.size() != scramble_size) return false;
  const auto masked = mask(verifier.reveal(), nonce);
  std::array<uint8_t, scramble_size> stage1{};
  for (size_t i = 0; i < scramble_size; ++i) stage1[i] = scramble[i] ^ masked.bytes[i];
  auto stage2 = dannan::identity::sha256(stage1);
  const bool same = dannan::identity::constant_time_equal(stage2.bytes, verifier.reveal());
  dannan::identity::cleanse(stage1);
  dannan::identity::cleanse(stage2.bytes);
  return same;
}

Secret read_password(std::span<const uint8_t> payload) {
  if (payload.empty() || payload.back() != 0) throw ProtocolError(er_malformed_packet, "Malformed communication packet.");
  const auto password = payload.first(payload.size() - 1);
  if (std::find(password.begin(), password.end(), uint8_t{0}) != password.end() || password.size() > max_password_bytes)
    throw ProtocolError(er_malformed_packet, "Malformed communication packet.");
  if (password.empty()) return Secret();
  return Secret(password);
}

}  // namespace kvasir::protocol::caching_sha2
