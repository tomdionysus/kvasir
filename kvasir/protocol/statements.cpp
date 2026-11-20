//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/protocol/statements.h"

#include <bit>
#include <charconv>
#include <cstring>
#include <stdexcept>

#include "kvasir/protocol/types.h"

namespace kvasir::protocol {

namespace {

namespace f = field_type;

[[noreturn]] void malformed() { throw ProtocolError(er_malformed_packet, "Malformed communication packet."); }

// How many bytes an integer of the type takes; 0 for a type that is not one.
int integer_width(uint8_t type) {
  switch (type) {
    case f::tiny:
      return 1;
    case f::short_:
    case f::year:
      return 2;
    case f::long_:
    case f::int24:
      return 4;
    case f::longlong:
      return 8;
    default:
      return 0;
  }
}

bool is_temporal(uint8_t type) { return type == f::date || type == f::datetime || type == f::timestamp || type == f::time; }

// The types a parameter can be sent as (the manual's Binary Protocol Value).
bool known(uint8_t type) {
  if (integer_width(type) || is_temporal(type)) return true;
  switch (type) {
    case f::float_:
    case f::double_:
    case f::null:
    case f::decimal:
    case f::varchar:
    case f::bit:
    case f::json:
    case f::newdecimal:
    case f::enum_:
    case f::set:
    case f::tiny_blob:
    case f::medium_blob:
    case f::long_blob:
    case f::blob:
    case f::var_string:
    case f::string:
    case f::geometry:
      return true;
    default:
      return false;
  }
}

BoundValue read_value(Reader& r, ValueType type) {
  BoundValue v;
  v.type = type;
  if (const int width = integer_width(type.type)) {
    const uint64_t raw = width == 1 ? r.u8() : width == 2 ? r.u16() : width == 4 ? r.u32() : r.u64();
    if (type.is_unsigned) {
      v.value = raw;
    } else {
      // Sign-extend from the value's width.
      const int shift = 64 - 8 * width;
      v.value = static_cast<int64_t>(raw << shift) >> shift;
    }
    return v;
  }
  if (type.type == f::float_) {
    v.value = static_cast<double>(std::bit_cast<float>(r.u32()));
    return v;
  }
  if (type.type == f::double_) {
    v.value = std::bit_cast<double>(r.u64());
    return v;
  }
  if (is_temporal(type.type)) {
    // A length byte, then that many bytes of fields.
    const auto bytes = r.view(r.u8());
    v.value = std::string(bytes.begin(), bytes.end());
    return v;
  }
  // Everything else is length-encoded; the packet itself bounds it.
  v.value = r.lenenc_string(r.remaining());
  return v;
}

int64_t signed_text(const std::string& text) {
  int64_t out = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  if (error != std::errc() || end != text.data() + text.size()) throw std::invalid_argument("not an integer: " + text);
  return out;
}

uint64_t unsigned_text(const std::string& text) {
  uint64_t out = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  if (error != std::errc() || end != text.data() + text.size()) throw std::invalid_argument("not an unsigned integer: " + text);
  return out;
}

double real_text(const std::string& text) {
  double out = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  if (error != std::errc() || end != text.data() + text.size()) throw std::invalid_argument("not a number: " + text);
  return out;
}

}  // namespace

Bytes encode(const PrepareOk& ok) {
  Writer w;
  w.u8(0x00);
  w.u32(ok.statement);
  w.u16(ok.columns);
  w.u16(ok.params);
  w.u8(0x00);
  w.u16(ok.warnings);
  return w.take();
}

uint32_t statement_of(std::span<const uint8_t> body) {
  Reader r(body);
  return r.u32();
}

Execute decode_execute(std::span<const uint8_t> body, std::vector<std::optional<ValueType>>& bound, std::span<const std::optional<std::string>> long_data) {
  if (long_data.size() != bound.size()) throw std::invalid_argument("long data for a different number of parameters");
  Reader r(body);
  Execute e;
  e.statement = r.u32();
  e.flags = r.u8();
  e.iterations = r.u32();
  const size_t count = bound.size();
  if (count == 0) {
    if (r.remaining() != 0) malformed();
    return e;
  }
  const auto nulls = r.view((count + 7) / 8);
  const uint8_t new_types = r.u8();
  if (new_types > 1) malformed();
  // Read every type before taking any, so a refused body changes nothing.
  std::vector<ValueType> types(count);
  for (size_t i = 0; i < count; ++i) {
    if (new_types) {
      const uint8_t type = r.u8();
      const uint8_t flags = r.u8();
      if (!known(type) || (flags & 0x7f)) malformed();
      types[i] = {type, (flags & 0x80) != 0};
    } else {
      if (!bound[i]) malformed();
      types[i] = *bound[i];
    }
  }
  e.params.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if ((nulls[i / 8] >> (i % 8)) & 1) {
      e.params.push_back({types[i], std::monostate{}});
    } else if (long_data[i]) {
      e.params.push_back({types[i], *long_data[i]});
    } else if (types[i].type == f::null) {
      e.params.push_back({types[i], std::monostate{}});
    } else {
      e.params.push_back(read_value(r, types[i]));
    }
  }
  if (r.remaining() != 0) malformed();
  for (size_t i = 0; i < count; ++i) bound[i] = types[i];
  return e;
}

Bytes encode_binary_row(std::span<const ValueType> columns, std::span<const std::optional<std::string>> values) {
  if (columns.size() != values.size()) throw std::invalid_argument("a row's values do not match its columns");
  Writer w;
  w.u8(0x00);
  std::vector<uint8_t> nulls((columns.size() + 7 + 2) / 8, 0);
  for (size_t i = 0; i < values.size(); ++i)
    if (!values[i]) nulls[(i + 2) / 8] = static_cast<uint8_t>(nulls[(i + 2) / 8] | (1u << ((i + 2) % 8)));
  w.raw(std::span<const uint8_t>(nulls));
  for (size_t i = 0; i < columns.size(); ++i) {
    if (!values[i]) continue;
    const auto& text = *values[i];
    const auto type = columns[i];
    if (const int width = integer_width(type.type)) {
      const uint64_t raw = type.is_unsigned ? unsigned_text(text) : static_cast<uint64_t>(signed_text(text));
      if (width == 1) w.u8(static_cast<uint8_t>(raw));
      if (width == 2) w.u16(static_cast<uint16_t>(raw));
      if (width == 4) w.u32(static_cast<uint32_t>(raw));
      if (width == 8) w.u64(raw);
    } else if (type.type == f::float_) {
      w.u32(std::bit_cast<uint32_t>(static_cast<float>(real_text(text))));
    } else if (type.type == f::double_) {
      w.u64(std::bit_cast<uint64_t>(real_text(text)));
    } else {
      w.lenenc_string(text);
    }
  }
  return w.take();
}

}  // namespace kvasir::protocol
