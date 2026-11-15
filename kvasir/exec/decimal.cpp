//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/decimal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace kvasir::exec {

namespace {

// Magnitudes: base 10^9, least significant word first, no high zero words.
using Magnitude = std::vector<uint32_t>;
constexpr uint32_t base = 1000000000;
constexpr int word_digits = 9;

void trim(Magnitude& m) {
  while (!m.empty() && m.back() == 0) m.pop_back();
}

int compare_magnitudes(const Magnitude& a, const Magnitude& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  for (size_t i = a.size(); i-- > 0;)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

Magnitude add(const Magnitude& a, const Magnitude& b) {
  Magnitude out;
  uint64_t carry = 0;
  for (size_t i = 0; i < std::max(a.size(), b.size()) || carry; ++i) {
    uint64_t sum = carry;
    if (i < a.size()) sum += a[i];
    if (i < b.size()) sum += b[i];
    out.push_back(static_cast<uint32_t>(sum % base));
    carry = sum / base;
  }
  trim(out);
  return out;
}

// a - b, for a >= b.
Magnitude subtract(const Magnitude& a, const Magnitude& b) {
  Magnitude out(a);
  int64_t borrow = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    int64_t v = static_cast<int64_t>(out[i]) - borrow - (i < b.size() ? static_cast<int64_t>(b[i]) : 0);
    borrow = v < 0 ? 1 : 0;
    if (v < 0) v += base;
    out[i] = static_cast<uint32_t>(v);
  }
  trim(out);
  return out;
}

Magnitude multiply(const Magnitude& a, const Magnitude& b) {
  if (a.empty() || b.empty()) return {};
  std::vector<uint64_t> wide(a.size() + b.size() + 1, 0);
  for (size_t i = 0; i < a.size(); ++i) {
    uint64_t carry = 0;
    for (size_t j = 0; j < b.size() || carry; ++j) {
      uint64_t cur = wide[i + j] + carry + (j < b.size() ? static_cast<uint64_t>(a[i]) * b[j] : 0);
      wide[i + j] = cur % base;
      carry = cur / base;
    }
  }
  Magnitude out(wide.begin(), wide.end());
  trim(out);
  return out;
}

Magnitude multiply_small(const Magnitude& a, uint32_t m) {
  Magnitude out;
  uint64_t carry = 0;
  for (size_t i = 0; i < a.size() || carry; ++i) {
    const uint64_t cur = carry + (i < a.size() ? static_cast<uint64_t>(a[i]) * m : 0);
    out.push_back(static_cast<uint32_t>(cur % base));
    carry = cur / base;
  }
  trim(out);
  return out;
}

Magnitude power_of_ten(int k) {
  Magnitude out{1};
  while (k > 0) {
    const int step = std::min(k, word_digits);
    uint32_t factor = 1;
    for (int i = 0; i < step; ++i) factor *= 10;
    out = multiply_small(out, factor);
    k -= step;
  }
  return out;
}

Magnitude scale_up(const Magnitude& a, int k) { return k <= 0 ? a : multiply(a, power_of_ten(k)); }

// Quotient and remainder, for a non-zero divisor.
std::pair<Magnitude, Magnitude> divide_magnitudes(const Magnitude& a, const Magnitude& b) {
  Magnitude quotient(a.size(), 0);
  Magnitude rest;
  for (size_t i = a.size(); i-- > 0;) {
    rest.insert(rest.begin(), a[i]);
    trim(rest);
    // The largest digit q with b * q <= rest.
    uint32_t low = 0, high = base - 1;
    while (low < high) {
      const uint32_t mid = low + (high - low + 1) / 2;
      if (compare_magnitudes(multiply_small(b, mid), rest) <= 0)
        low = mid;
      else
        high = mid - 1;
    }
    quotient[i] = low;
    if (low) rest = subtract(rest, multiply_small(b, low));
  }
  trim(quotient);
  return {quotient, rest};
}

std::string digits_of(const Magnitude& m) {
  if (m.empty()) return "0";
  std::string out = std::to_string(m.back());
  for (size_t i = m.size() - 1; i-- > 0;) {
    const auto word = std::to_string(m[i]);
    out.append(static_cast<size_t>(word_digits) - word.size(), '0');
    out += word;
  }
  return out;
}

Magnitude magnitude_of(std::string_view digits) {
  Magnitude out;
  size_t end = digits.size();
  while (end > 0) {
    const size_t start = end > static_cast<size_t>(word_digits) ? end - word_digits : 0;
    uint32_t word = 0;
    for (size_t i = start; i < end; ++i) word = word * 10 + static_cast<uint32_t>(digits[i] - '0');
    out.push_back(word);
    end = start;
  }
  trim(out);
  return out;
}

size_t digit_count(const Magnitude& m) { return m.empty() ? 1 : digits_of(m).size(); }

int round_up_words(int digits) { return (digits + word_digits - 1) / word_digits * word_digits; }

}  // namespace

std::optional<Decimal> Decimal::parse(std::string_view text) {
  bool negative = false;
  if (!text.empty() && text[0] == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  const auto point = text.find('.');
  const auto whole = text.substr(0, point);
  const auto fraction = point == std::string_view::npos ? std::string_view() : text.substr(point + 1);
  if (whole.empty() && fraction.empty()) return std::nullopt;
  for (const auto part : {whole, fraction})
    for (const char c : part)
      if (c < '0' || c > '9') return std::nullopt;
  if (whole.size() + fraction.size() > static_cast<size_t>(decimal_max_digits)) return std::nullopt;
  Decimal out;
  out._words = magnitude_of(std::string(whole) + std::string(fraction));
  out._frac = static_cast<int>(fraction.size());
  out._negative = negative && !out._words.empty();
  return out;
}

Decimal Decimal::from_int(int64_t value) {
  Decimal out;
  const uint64_t magnitude = value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
  out = from_uint(magnitude);
  out._negative = value < 0;
  return out;
}

Decimal Decimal::from_uint(uint64_t value) {
  Decimal out;
  while (value) {
    out._words.push_back(static_cast<uint32_t>(value % base));
    value /= base;
  }
  return out;
}

bool Decimal::is_zero() const noexcept { return _words.empty(); }

int Decimal::int_digits() const {
  const int digits = static_cast<int>(digits_of(_words).size());
  return std::max(1, digits - _frac);
}

std::string Decimal::to_string() const {
  std::string digits = digits_of(_words);
  if (_frac > 0) {
    if (digits.size() <= static_cast<size_t>(_frac)) digits.insert(0, static_cast<size_t>(_frac) + 1 - digits.size(), '0');
    digits.insert(digits.size() - static_cast<size_t>(_frac), ".");
  }
  return _negative ? "-" + digits : digits;
}

Decimal Decimal::rounded(int frac) const {
  if (frac >= _frac) {
    Decimal out = *this;
    out._words = scale_up(_words, frac - _frac);
    out._frac = frac;
    return out;
  }
  const auto divisor = power_of_ten(_frac - frac);
  auto [q, r] = divide_magnitudes(_words, divisor);
  if (compare_magnitudes(multiply_small(r, 2), divisor) >= 0) q = add(q, Magnitude{1});
  Decimal out;
  out._words = q;
  out._frac = frac;
  out._negative = _negative && !q.empty();
  return out;
}

Decimal Decimal::truncated(int frac) const {
  if (frac >= _frac) return rounded(frac);
  Decimal out;
  out._words = divide_magnitudes(_words, power_of_ten(_frac - frac)).first;
  out._frac = frac;
  out._negative = _negative && !out._words.empty();
  return out;
}

Decimal Decimal::negated() const {
  Decimal out = *this;
  out._negative = !_negative && !_words.empty();
  return out;
}

namespace {

void check_size(const std::vector<uint32_t>& words) {
  if (digit_count(words) > static_cast<size_t>(decimal_max_digits)) throw std::overflow_error("DECIMAL value is out of range");
}

}  // namespace

Decimal operator+(const Decimal& a, const Decimal& b) {
  const int frac = std::max(a._frac, b._frac);
  const auto x = scale_up(a._words, frac - a._frac);
  const auto y = scale_up(b._words, frac - b._frac);
  Decimal out;
  out._frac = frac;
  if (a._negative == b._negative) {
    out._words = add(x, y);
    out._negative = a._negative;
  } else if (compare_magnitudes(x, y) >= 0) {
    out._words = subtract(x, y);
    out._negative = a._negative;
  } else {
    out._words = subtract(y, x);
    out._negative = b._negative;
  }
  if (out._words.empty()) out._negative = false;
  check_size(out._words);
  return out;
}

Decimal operator-(const Decimal& a, const Decimal& b) { return a + b.negated(); }

Decimal operator*(const Decimal& a, const Decimal& b) {
  Decimal out;
  out._words = multiply(a._words, b._words);
  out._frac = a._frac + b._frac;
  out._negative = (a._negative != b._negative) && !out._words.empty();
  if (out._frac > decimal_max_scale) out = out.rounded(decimal_max_scale);
  check_size(out._words);
  return out;
}

std::optional<Decimal> Decimal::divide(const Decimal& a, const Decimal& b, int increment) {
  if (b._words.empty()) return std::nullopt;
  const int frac1 = round_up_words(a._frac);
  const int frac2 = round_up_words(b._frac);
  const int extra = std::max(0, increment - (frac1 - a._frac) - (frac2 - b._frac));
  int frac = round_up_words(frac1 + frac2 + extra);
  // Within the digits arithmetic carries: the fraction gives way.
  const auto whole = divide_magnitudes(scale_up(a._words, b._frac), scale_up(b._words, a._frac)).first;
  const int whole_digits = whole.empty() ? 0 : static_cast<int>(digit_count(whole));
  frac = std::min(frac, decimal_max_digits - whole_digits);
  Decimal out;
  out._words = divide_magnitudes(scale_up(a._words, frac - a._frac + b._frac), b._words).first;
  out._frac = frac;
  out._negative = (a._negative != b._negative) && !out._words.empty();
  check_size(out._words);
  return out;
}

std::optional<Decimal> Decimal::remainder(const Decimal& a, const Decimal& b) {
  if (b._words.empty()) return std::nullopt;
  const int frac = std::max(a._frac, b._frac);
  Decimal out;
  out._words = divide_magnitudes(scale_up(a._words, frac - a._frac), scale_up(b._words, frac - b._frac)).second;
  out._frac = frac;
  out._negative = a._negative && !out._words.empty();
  return out;
}

int compare(const Decimal& a, const Decimal& b) {
  if (a._negative != b._negative) return a._negative ? -1 : 1;
  const int frac = std::max(a._frac, b._frac);
  const int c = compare_magnitudes(scale_up(a._words, frac - a._frac), scale_up(b._words, frac - b._frac));
  return a._negative ? -c : c;
}

double Decimal::to_double() const {
  const auto text = to_string();
  double value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

std::optional<int64_t> Decimal::to_int() const {
  const auto whole = truncated(0);
  if (whole._words.size() > 3) return std::nullopt;
  uint64_t magnitude = 0;
  for (size_t i = whole._words.size(); i-- > 0;) {
    if (magnitude > (UINT64_MAX - whole._words[i]) / base) return std::nullopt;
    magnitude = magnitude * base + whole._words[i];
  }
  if (_negative) {
    if (magnitude > static_cast<uint64_t>(INT64_MAX) + 1) return std::nullopt;
    return static_cast<int64_t>(0 - magnitude);
  }
  if (magnitude > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
  return static_cast<int64_t>(magnitude);
}

std::string format_double(double value) {
  if (value == 0) return std::signbit(value) ? "-0" : "0";
  char buffer[64];
  const auto written = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
  const std::string_view text(buffer, static_cast<size_t>(written.ptr - buffer));
  // [-]d[.ddd]e(+|-)xx
  const bool negative = text[0] == '-';
  const auto e = text.find('e');
  std::string digits;
  for (const char c : text.substr(negative ? 1 : 0, e - (negative ? 1 : 0)))
    if (c != '.') digits.push_back(c);
  int exponent = 0;
  std::from_chars(text.data() + e + (text[e + 1] == '+' ? 2 : 1), text.data() + text.size(), exponent);
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

  std::string out = negative ? "-" : "";
  if (exponent >= -15 && exponent <= 14) {
    const int point = exponent + 1;
    if (point <= 0) {
      out += "0." + std::string(static_cast<size_t>(-point), '0') + digits;
    } else if (static_cast<size_t>(point) >= digits.size()) {
      out += digits + std::string(static_cast<size_t>(point) - digits.size(), '0');
    } else {
      out += digits.substr(0, static_cast<size_t>(point)) + "." + digits.substr(static_cast<size_t>(point));
    }
  } else {
    out += digits.substr(0, 1);
    if (digits.size() > 1) out += "." + digits.substr(1);
    out += "e" + std::to_string(exponent);
  }
  return out;
}

}  // namespace kvasir::exec
