//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// COM_STMT_EXECUTE's reader over arbitrary bodies, for a statement of up to
// 64 parameters, some with long data, and again with the types the first
// body bound: each returns a value or refuses with a ProtocolError, never
// anything else, with one value per parameter.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kvasir/protocol/statements.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) return 0;
  // The first byte is the parameter count, the second which have long data.
  const size_t count = data[0] % 65;
  std::vector<std::optional<kvasir::protocol::ValueType>> bound(count);
  std::vector<std::optional<std::string>> long_data(count);
  for (size_t i = 0; i < count; ++i)
    if ((data[1] >> (i % 8)) & 1 && i % 3 == 0) long_data[i] = "long";
  const std::span<const uint8_t> body(data + 2, size - 2);
  for (int round = 0; round < 2; ++round) {
    try {
      const auto e = kvasir::protocol::decode_execute(body, bound, long_data);
      if (e.params.size() != count) std::abort();
    } catch (const kvasir::protocol::ProtocolError&) {
    }
  }
  return 0;
}
