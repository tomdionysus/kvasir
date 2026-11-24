//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/protocol/statements.h"

#include <bit>
#include <charconv>
#include <cstdio>
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
    // A length byte, then that many bytes of fields: 0, 4, 7 or 11 for a
    // date, 0, 8 or 12 for a time.
    const uint8_t length = r.u8();
    const bool fits = type.type == f::time ? (length == 0 || length == 8 || length == 12) : (length == 0 || length == 4 || length == 7 || length == 11);
    if (!fits) malformed();
    const auto bytes = r.view(length);
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

// A temporal value's text as Kvasir carries it (2024-02-29, 2024-02-29
// 12:34:56.789, -26:02:03.5) written as the binary protocol's fields.
void write_temporal(Writer& w, uint8_t type, const std::string& text) {
  size_t i = 0;
  const auto bad = [&]() -> void { throw std::invalid_argument("not a " + std::string(type == f::time ? "time" : "date") + ": " + text); };
  // Digits up to the next non-digit, at least one.
  const auto number = [&](size_t most) {
    uint64_t v = 0;
    const size_t start = i;
    while (i < text.size() && i - start < most && text[i] >= '0' && text[i] <= '9') v = v * 10 + static_cast<uint64_t>(text[i++] - '0');
    if (i == start) bad();
    return v;
  };
  const auto expect = [&](char ch) {
    if (i >= text.size() || text[i] != ch) bad();
    ++i;
  };
  // The fraction as microseconds, its digits scaled to six.
  const auto fraction = [&] {
    if (i >= text.size()) return uint64_t{0};
    expect('.');
    const size_t start = i;
    uint64_t micro = number(6);
    for (size_t k = i - start; k < 6; ++k) micro *= 10;
    return micro;
  };
  if (type == f::time) {
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) ++i;
    const uint64_t hours = number(9);
    expect(':');
    const uint64_t minute = number(2);
    expect(':');
    const uint64_t second = number(2);
    const uint64_t micro = fraction();
    if (i != text.size()) bad();
    const bool zero = !hours && !minute && !second && !micro;
    const uint8_t length = zero ? 0 : micro ? 12 : 8;
    w.u8(length);
    if (!length) return;
    w.u8(negative ? 1 : 0);
    w.u32(static_cast<uint32_t>(hours / 24));
    w.u8(static_cast<uint8_t>(hours % 24));
    w.u8(static_cast<uint8_t>(minute));
    w.u8(static_cast<uint8_t>(second));
    if (length == 12) w.u32(static_cast<uint32_t>(micro));
    return;
  }
  const uint64_t year = number(4);
  expect('-');
  const uint64_t month = number(2);
  expect('-');
  const uint64_t day = number(2);
  uint64_t hour = 0, minute = 0, second = 0, micro = 0;
  if (i < text.size()) {
    expect(' ');
    hour = number(2);
    expect(':');
    minute = number(2);
    expect(':');
    second = number(2);
    micro = fraction();
  }
  if (i != text.size()) bad();
  const uint8_t length = micro ? 11 : (hour || minute || second) ? 7 : (year || month || day) ? 4 : 0;
  w.u8(length);
  if (!length) return;
  w.u16(static_cast<uint16_t>(year));
  w.u8(static_cast<uint8_t>(month));
  w.u8(static_cast<uint8_t>(day));
  if (length < 7) return;
  w.u8(static_cast<uint8_t>(hour));
  w.u8(static_cast<uint8_t>(minute));
  w.u8(static_cast<uint8_t>(second));
  if (length == 11) w.u32(static_cast<uint32_t>(micro));
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

std::string temporal_text(uint8_t type, std::string_view fields) {
  const auto byte = [&](size_t i) { return i < fields.size() ? static_cast<uint32_t>(static_cast<uint8_t>(fields[i])) : 0u; };
  const auto u32_at = [&](size_t i) { return byte(i) | byte(i + 1) << 8 | byte(i + 2) << 16 | byte(i + 3) << 24; };
  char buffer[64];
  if (type == f::time) {
    const bool negative = byte(0) != 0;
    uint64_t hours = static_cast<uint64_t>(u32_at(1)) * 24 + byte(5);
    uint32_t minute = byte(6), second = byte(7), micro = fields.size() >= 12 ? u32_at(8) : 0;
    if (hours > 838 || (hours == 838 && (minute > 59 || second > 59))) {
      hours = 838;
      minute = second = 59;
      micro = 0;
    }
    std::snprintf(buffer, sizeof buffer, "%s%02llu:%02u:%02u.%06u", negative && (hours || minute || second || micro) ? "-" : "",
                  static_cast<unsigned long long>(hours), minute, second, micro);
    return buffer;
  }
  const uint32_t year = byte(0) | byte(1) << 8, month = byte(2), day = byte(3);
  if (type == f::date) {
    std::snprintf(buffer, sizeof buffer, "%04u-%02u-%02u", year, month, day);
    return buffer;
  }
  const uint32_t micro = fields.size() >= 11 ? u32_at(7) : 0;
  std::snprintf(buffer, sizeof buffer, "%04u-%02u-%02u %02u:%02u:%02u.%06u", year, month, day, byte(4), byte(5), byte(6), micro);
  return buffer;
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
    } else if (is_temporal(type.type)) {
      write_temporal(w, type.type, text);
    } else {
      w.lenenc_string(text);
    }
  }
  return w.take();
}

}  // namespace kvasir::protocol
