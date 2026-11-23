//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/temporal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>

namespace kvasir::exec::temporal {

uint32_t days_in_month(uint32_t year, uint32_t month) {
  static constexpr uint32_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : days[month - 1];
}

namespace {

// TIME's limit: 838:59:59.
constexpr uint32_t max_hours = 838;
constexpr int64_t day_us = 86400LL * 1000000;
// TO_DAYS of 1970-01-01.
constexpr int64_t days_to_epoch = 719528;

bool digit(char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; }
bool space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

// A two-digit year as MySQL reads it: 70-99 the 1900s, 00-69 the 2000s.
uint32_t full_year(uint32_t year) { return year < 70 ? 2000 + year : 1900 + year; }

uint32_t number(std::string_view s) {
  uint32_t v = 0;
  for (const char ch : s) v = v * 10 + static_cast<uint32_t>(ch - '0');
  return v;
}

// The day after.
void next_day(Moment& m) {
  if (++m.day <= days_in_month(m.year, m.month)) return;
  m.day = 1;
  if (++m.month <= 12) return;
  m.month = 1;
  ++m.year;
}

// A fraction's digits rounded to `fsp` (MySQL rounds half up on the digit
// after), as microseconds; whether it rounded up past a whole second.
uint32_t fraction(std::string_view digits, int fsp, bool* carry) {
  *carry = false;
  if (fsp < 0) fsp = 6;
  std::string kept(digits.substr(0, static_cast<size_t>(fsp)));
  kept.resize(6, '0');
  uint32_t micro = number(kept);
  if (digits.size() > static_cast<size_t>(fsp) && digits[static_cast<size_t>(fsp)] >= '5') {
    uint32_t step = 1;
    for (int i = fsp; i < 6; ++i) step *= 10;
    micro += step;
    if (micro >= 1000000) {
      micro = 0;
      *carry = true;
    }
  }
  return micro;
}

// A second carried into a DATETIME's minutes, hours and days.
void carry_second(Moment& m, bool time_of_day) {
  if (++m.second < 60) return;
  m.second = 0;
  if (++m.minute < 60) return;
  m.minute = 0;
  ++m.hour;
  if (time_of_day && m.hour == 24) {
    m.hour = 0;
    next_day(m);
  }
}

bool valid_time_of_day(const Moment& m) { return m.hour < 24 && m.minute < 60 && m.second < 60; }

// The rest after a value: blanks alone are superfluous delimiters (4096,
// at the first); anything else is truncation, a blank before it superfluous
// too (observed).
void rest(std::string_view s, size_t at, Read& out) {
  if (at >= s.size()) return;
  if (space(s[at])) out.superfluous.push_back(at);
  size_t i = at;
  while (i < s.size() && space(s[i])) ++i;
  if (i < s.size()) out.truncated = true;
}

}  // namespace

bool valid_date(uint32_t year, uint32_t month, uint32_t day) {
  return year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
}

Read read_datetime(std::string_view s, int fsp) {
  Read out;
  size_t i = 0;
  while (i < s.size() && space(s[i])) {
    if (out.superfluous.empty()) out.superfluous.push_back(i);
    ++i;
  }
  Moment m;
  std::string_view frac;
  // Digits alone (and a fraction): the fields by length.
  size_t run = i;
  while (run < s.size() && digit(s[run])) ++run;
  const size_t length = run - i;
  const bool bare = run == s.size() || s[run] == '.' || space(s[run]);
  if (bare && length >= 6 && length != 7 && length != 9 && length != 10 && length != 11 && length != 13 && length <= 14) {
    const auto d = s.substr(i, length);
    const bool long_year = length == 8 || length == 14;
    const size_t y = long_year ? 4 : 2;
    m.year = long_year ? number(d.substr(0, 4)) : full_year(number(d.substr(0, 2)));
    m.month = number(d.substr(y, 2));
    m.day = number(d.substr(y + 2, 2));
    if (length >= 12) {
      out.has_time = true;
      m.hour = number(d.substr(y + 4, 2));
      m.minute = number(d.substr(y + 6, 2));
      m.second = number(d.substr(y + 8, 2));
    }
    i = run;
    if (length >= 12 && i < s.size() && s[i] == '.') {
      size_t j = i + 1;
      while (j < s.size() && digit(s[j])) ++j;
      frac = s.substr(i + 1, j - i - 1);
      i = j;
    }
  } else {
    // Delimited: year, month, day by any punctuation, then a space or 'T'
    // and hour, minute, second by any punctuation, then '.' and a fraction.
    uint32_t fields[6] = {0, 0, 0, 0, 0, 0};
    size_t count = 0;
    size_t year_digits = 0;
    while (count < 6 && i < s.size() && digit(s[i])) {
      size_t j = i;
      while (j < s.size() && digit(s[j]) && j - i < (count == 0 ? 4u : 2u)) ++j;
      if (count == 0) year_digits = j - i;
      fields[count++] = number(s.substr(i, j - i));
      i = j;
      if (i >= s.size() || digit(s[i])) break;
      // The fraction follows the seconds.
      if (count == 6 && s[i] == '.') break;
      const char d = s[i];
      const bool to_time = count == 3;
      if (to_time ? !(space(d) || d == 'T') && !std::ispunct(static_cast<unsigned char>(d)) : !std::ispunct(static_cast<unsigned char>(d))) break;
      if (i + 1 >= s.size() || !digit(s[i + 1])) break;
      ++i;
    }
    if (count < 3) return out;
    out.has_time = count > 3;
    m.year = year_digits <= 2 ? full_year(fields[0]) : fields[0];
    m.month = fields[1];
    m.day = fields[2];
    m.hour = fields[3];
    m.minute = fields[4];
    m.second = fields[5];
    if (count == 6 && i < s.size() && s[i] == '.') {
      size_t j = i + 1;
      while (j < s.size() && digit(s[j])) ++j;
      frac = s.substr(i + 1, j - i - 1);
      i = j;
    }
  }
  rest(s, i, out);
  if (!valid_date(m.year, m.month, m.day) || !valid_time_of_day(m)) return out;
  bool carry = false;
  m.micro = fraction(frac, fsp, &carry);
  if (carry) carry_second(m, true);
  if (m.year > 9999) return out;
  out.moment = m;
  return out;
}

Read read_time(std::string_view s, int fsp) {
  Read out;
  size_t i = 0;
  while (i < s.size() && space(s[i])) {
    if (out.superfluous.empty()) out.superfluous.push_back(i);
    ++i;
  }
  // A date and time: its time.
  {
    size_t j = i;
    while (j < s.size() && digit(s[j])) ++j;
    if ((j - i == 4 && j < s.size() && (s[j] == '-' || s[j] == '/')) || (j - i >= 12 && (j == s.size() || s[j] == '.'))) {
      auto dt = read_datetime(s, fsp);
      if (dt.moment) {
        dt.moment->year = dt.moment->month = dt.moment->day = 0;
      }
      return dt;
    }
  }
  Moment m;
  if (i < s.size() && s[i] == '-') {
    m.negative = true;
    ++i;
  }
  const auto digits_at = [&](size_t at) {
    size_t j = at;
    while (j < s.size() && digit(s[j])) ++j;
    return j;
  };
  std::string_view frac;
  uint64_t days = 0;
  size_t j = digits_at(i);
  if (j == i) return out;
  // D hh[:mm[:ss]]: days before a space.
  if (j < s.size() && s[j] == ' ' && j + 1 < s.size() && digit(s[j + 1])) {
    days = number(s.substr(i, std::min<size_t>(j - i, 8)));
    i = j + 1;
    j = digits_at(i);
  }
  if (j < s.size() && s[j] == ':') {
    m.hour = number(s.substr(i, std::min<size_t>(j - i, 8)));
    i = j + 1;
    j = digits_at(i);
    m.minute = number(s.substr(i, std::min<size_t>(j - i, 8)));
    i = j;
    if (i < s.size() && s[i] == ':' && i + 1 < s.size() && digit(s[i + 1])) {
      j = digits_at(i + 1);
      m.second = number(s.substr(i + 1, std::min<size_t>(j - i - 1, 8)));
      i = j;
    }
  } else if (days) {
    m.hour = number(s.substr(i, std::min<size_t>(j - i, 8)));
    i = j;
  } else {
    // Digits from the right: ss, mmss, hhmmss.
    const auto d = s.substr(i, j - i);
    const size_t n = d.size();
    m.second = number(d.substr(n >= 2 ? n - 2 : 0));
    if (n > 2) m.minute = number(d.substr(n >= 4 ? n - 4 : 0, n >= 4 ? 2 : n - 2));
    if (n > 4) m.hour = number(d.substr(0, std::min<size_t>(n - 4, 8)));
    i = j;
  }
  if (i < s.size() && s[i] == '.') {
    j = digits_at(i + 1);
    frac = s.substr(i + 1, j - i - 1);
    i = j;
  }
  rest(s, i, out);
  if (m.minute > 59 || m.second > 59) return out;
  bool carry = false;
  m.micro = fraction(frac, fsp, &carry);
  if (carry) carry_second(m, false);
  const uint64_t hours = days * 24 + m.hour;
  if (hours > max_hours) {
    m.hour = max_hours;
    m.minute = 59;
    m.second = 59;
    m.micro = 0;
    out.clamped = true;
  } else {
    m.hour = static_cast<uint32_t>(hours);
  }
  out.moment = m;
  return out;
}

std::optional<Moment> datetime_from_number(uint64_t n, uint32_t micro, bool negative, int fsp) {
  if (negative || n == 0) return std::nullopt;
  Moment m;
  uint64_t date = n;
  uint64_t time = 0;
  bool has_time = false;
  if (n < 101) return std::nullopt;
  if (n <= 691231) {
    date = n + 20000000;
  } else if (n < 700101) {
    return std::nullopt;
  } else if (n <= 991231) {
    date = n + 19000000;
  } else if (n < 10000101) {
    return std::nullopt;
  } else if (n <= 99991231) {
    date = n;
  } else if (n < 101000000) {
    return std::nullopt;
  } else if (n <= 691231235959) {
    date = n / 1000000 + 20000000;
    time = n % 1000000;
    has_time = true;
  } else if (n < 700101000000) {
    return std::nullopt;
  } else if (n <= 991231235959) {
    date = n / 1000000 + 19000000;
    time = n % 1000000;
    has_time = true;
  } else if (n < 10000101000000) {
    return std::nullopt;
  } else if (n <= 99991231235959) {
    date = n / 1000000;
    time = n % 1000000;
    has_time = true;
  } else {
    return std::nullopt;
  }
  m.year = static_cast<uint32_t>(date / 10000);
  m.month = static_cast<uint32_t>(date / 100 % 100);
  m.day = static_cast<uint32_t>(date % 100);
  m.hour = static_cast<uint32_t>(time / 10000);
  m.minute = static_cast<uint32_t>(time / 100 % 100);
  m.second = static_cast<uint32_t>(time % 100);
  if (!valid_date(m.year, m.month, m.day) || !valid_time_of_day(m)) return std::nullopt;
  if (has_time) {
    char digits[8];
    std::snprintf(digits, sizeof digits, "%06u", micro);
    bool carry = false;
    m.micro = fraction(digits, fsp, &carry);
    if (carry) carry_second(m, true);
  }
  return m;
}

std::optional<Moment> time_from_number(uint64_t n, uint32_t micro, bool negative, int fsp, bool* clamped) {
  *clamped = false;
  // A date and time's number: its time.
  if (n >= 10000000000ULL) {
    auto dt = datetime_from_number(n, micro, negative, fsp);
    if (dt) dt->year = dt->month = dt->day = 0;
    return dt;
  }
  Moment m;
  m.negative = negative;
  m.second = static_cast<uint32_t>(n % 100);
  m.minute = static_cast<uint32_t>(n / 100 % 100);
  const uint64_t hours = n / 10000;
  // A number past 838 hours is no TIME (observed: 8390000 is NULL), where a
  // string's is clamped.
  if (m.minute > 59 || m.second > 59 || hours > max_hours) return std::nullopt;
  char digits[8];
  std::snprintf(digits, sizeof digits, "%06u", micro);
  bool carry = false;
  m.micro = fraction(digits, fsp, &carry);
  m.hour = static_cast<uint32_t>(std::min<uint64_t>(hours, max_hours + 1));
  if (carry) carry_second(m, false);
  if (hours > max_hours || m.hour > max_hours) {
    m.hour = max_hours;
    m.minute = 59;
    m.second = 59;
    m.micro = 0;
    *clamped = true;
  }
  return m;
}

std::optional<Moment> from_number(std::string_view text, Kind kind, int fsp) {
  const bool negative = !text.empty() && text[0] == '-';
  if (negative) text.remove_prefix(1);
  const auto point = text.find('.');
  const auto whole = text.substr(0, point);
  std::string micro(point == std::string_view::npos ? std::string_view() : text.substr(point + 1));
  micro.resize(6, '0');
  if (whole.empty() || whole.size() > 19) return std::nullopt;
  uint64_t integer = 0;
  for (const char ch : whole) {
    if (!digit(ch)) return std::nullopt;
    integer = integer * 10 + static_cast<uint64_t>(ch - '0');
  }
  if (kind == Kind::time) {
    bool clamped = false;
    return time_from_number(integer, number(micro), negative, fsp, &clamped);
  }
  return datetime_from_number(integer, number(micro), negative, kind == Kind::date ? 6 : fsp);
}

void round(Moment& m, Kind kind, int fsp) {
  if (kind == Kind::date) {
    m.hour = m.minute = m.second = m.micro = 0;
    return;
  }
  char digits[8];
  std::snprintf(digits, sizeof digits, "%06u", m.micro);
  bool carry = false;
  m.micro = fraction(digits, fsp, &carry);
  if (carry) carry_second(m, kind == Kind::datetime);
  if (kind == Kind::time && m.hour > max_hours) {
    m.hour = max_hours;
    m.minute = 59;
    m.second = 59;
    m.micro = 0;
  }
}

std::string text(const Moment& m, Kind kind, int fsp) {
  char buffer[64];
  std::string out;
  if (kind != Kind::time) {
    std::snprintf(buffer, sizeof buffer, "%04u-%02u-%02u", m.year, m.month, m.day);
    out = buffer;
    if (kind == Kind::date) return out;
    out += ' ';
  } else if (m.negative) {
    out += '-';
  }
  std::snprintf(buffer, sizeof buffer, "%02u:%02u:%02u", m.hour, m.minute, m.second);
  out += buffer;
  if (fsp > 0) {
    std::snprintf(buffer, sizeof buffer, "%06u", m.micro);
    out += '.';
    out.append(buffer, static_cast<size_t>(fsp));
  }
  return out;
}

Moment parse_text(std::string_view s, Kind kind) {
  Moment m;
  size_t i = 0;
  const auto field = [&](size_t limit) {
    uint32_t v = 0;
    size_t n = 0;
    while (i < s.size() && digit(s[i]) && n < limit) {
      v = v * 10 + static_cast<uint32_t>(s[i++] - '0');
      ++n;
    }
    if (i < s.size() && !digit(s[i])) ++i;
    return v;
  };
  if (kind != Kind::time) {
    m.year = field(4);
    m.month = field(2);
    m.day = field(2);
    if (kind == Kind::date) return m;
  } else if (i < s.size() && s[i] == '-') {
    m.negative = true;
    ++i;
  }
  m.hour = field(3);
  m.minute = field(2);
  m.second = field(2);
  if (i <= s.size() && i > 0 && s[i - 1] == '.') {
    std::string digits;
    while (i < s.size() && digit(s[i])) digits.push_back(s[i++]);
    digits.resize(6, '0');
    m.micro = number(digits);
  }
  return m;
}

std::string number_text(const Moment& m, Kind kind, int fsp) {
  char buffer[64];
  std::string out;
  if (kind == Kind::date) {
    std::snprintf(buffer, sizeof buffer, "%u%02u%02u", m.year, m.month, m.day);
    return buffer;
  }
  if (kind == Kind::datetime) {
    std::snprintf(buffer, sizeof buffer, "%u%02u%02u%02u%02u%02u", m.year, m.month, m.day, m.hour, m.minute, m.second);
  } else {
    std::snprintf(buffer, sizeof buffer, "%s%u", m.negative ? "-" : "", m.hour * 10000 + m.minute * 100 + m.second);
  }
  out = buffer;
  if (fsp > 0) {
    std::snprintf(buffer, sizeof buffer, "%06u", m.micro);
    out += '.';
    out.append(buffer, static_cast<size_t>(fsp));
  }
  return out;
}

Moment from_unix_us(int64_t us) {
  constexpr int64_t per_day = 86400LL * 1000000;
  int64_t days = us / per_day;
  int64_t rest = us % per_day;
  if (rest < 0) {
    rest += per_day;
    --days;
  }
  // Days since 1970-01-01 as a civil date (Howard Hinnant's algorithm).
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t mo = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = yoe + era * 400 + (mo <= 2 ? 1 : 0);
  Moment m;
  m.year = static_cast<uint32_t>(y);
  m.month = static_cast<uint32_t>(mo);
  m.day = static_cast<uint32_t>(d);
  const int64_t seconds = rest / 1000000;
  m.hour = static_cast<uint32_t>(seconds / 3600);
  m.minute = static_cast<uint32_t>(seconds / 60 % 60);
  m.second = static_cast<uint32_t>(seconds % 60);
  m.micro = static_cast<uint32_t>(rest % 1000000);
  return m;
}

int64_t to_unix_us(const Moment& m) {
  // A civil date as days since 1970-01-01 (Howard Hinnant's algorithm).
  const int64_t y = static_cast<int64_t>(m.year) - (m.month <= 2 ? 1 : 0);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t mp = m.month > 2 ? m.month - 3 : m.month + 9;
  const int64_t doy = (153 * mp + 2) / 5 + m.day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + doe - 719468;
  const int64_t seconds = days * 86400 + static_cast<int64_t>(m.hour) * 3600 + m.minute * 60 + m.second;
  return seconds * 1000000 + m.micro;
}

int64_t day_number(const Moment& m) {
  Moment day{false, m.year, m.month, m.day, 0, 0, 0, 0};
  return to_unix_us(day) / day_us + days_to_epoch;
}

Moment from_day_number(int64_t day) {
  if (day <= 365) return Moment{};
  return from_unix_us((day - days_to_epoch) * day_us);
}

namespace {

// MySQL's days in a year: year 0 is no leap year.
int64_t days_in_year(int64_t year) { return (year & 3) == 0 && (year % 100 || (year % 400 == 0 && year)) ? 366 : 365; }

// The weekday of a day number, from Monday (0) or from Sunday.
int64_t weekday_of(int64_t day, bool from_sunday) { return (day + 5 + (from_sunday ? 1 : 0)) % 7; }

}  // namespace

uint32_t week(const Moment& m, uint32_t behaviour, uint32_t* year) {
  const int64_t day = day_number(m);
  int64_t first = day_number(Moment{false, m.year, 1, 1, 0, 0, 0, 0});
  const bool monday_first = behaviour & week_monday_first;
  bool by_year = behaviour & week_year;
  const bool first_weekday = behaviour & week_first_weekday;
  int64_t weekday = weekday_of(first, !monday_first);
  int64_t y = m.year;
  const auto late_start = [&] { return first_weekday ? weekday != 0 : weekday >= 4; };
  if (m.month == 1 && m.day <= 7 - weekday) {
    if (!by_year && late_start()) {
      *year = static_cast<uint32_t>(y);
      return 0;
    }
    by_year = true;
    --y;
    const int64_t days = days_in_year(y);
    first -= days;
    weekday = (weekday + 53 * 7 - days) % 7;
  }
  const int64_t into = late_start() ? day - (first + (7 - weekday)) : day - (first - weekday);
  if (by_year && into >= 52 * 7) {
    weekday = (weekday + days_in_year(y)) % 7;
    if ((!first_weekday && weekday < 4) || (first_weekday && weekday == 0)) {
      *year = static_cast<uint32_t>(y + 1);
      return 1;
    }
  }
  *year = static_cast<uint32_t>(y);
  return static_cast<uint32_t>(into / 7 + 1);
}

uint32_t week_mode(int64_t mode) {
  uint32_t behaviour = static_cast<uint32_t>(static_cast<uint64_t>(mode) & 7);
  if (!(behaviour & week_monday_first)) behaviour ^= week_first_weekday;
  return behaviour;
}

std::string_view weekday_name(uint32_t from_monday) {
  static constexpr std::string_view names[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
  return names[from_monday % 7];
}

std::string_view month_name(uint32_t month) {
  static constexpr std::string_view names[] = {"January", "February", "March",     "April",   "May",      "June",
                                               "July",    "August",   "September", "October", "November", "December"};
  return names[(month + 11) % 12];
}

std::optional<std::string> format(const Moment& m, std::string_view f, bool time_only) {
  std::string out;
  if (m.negative) out += '-';
  char buffer[32];
  const auto padded = [&](int64_t v, int width) {
    std::snprintf(buffer, sizeof buffer, "%0*lld", width, static_cast<long long>(v));
    out += buffer;
  };
  const uint32_t hour12 = (m.hour % 24 + 11) % 12 + 1;
  const char* meridiem = m.hour % 24 < 12 ? "AM" : "PM";
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] != '%' || i + 1 == f.size()) {
      out += f[i];
      continue;
    }
    const char spec = f[++i];
    // Specifiers that need a date: NULL for TIME_FORMAT.
    if (time_only && std::string_view("WaDjUuVvXxw").find(spec) != std::string_view::npos) return std::nullopt;
    uint32_t week_year_out = 0;
    switch (spec) {
      case 'M':
        if (!m.month) return std::nullopt;
        out += month_name(m.month);
        break;
      case 'b':
        if (!m.month) return std::nullopt;
        out += month_name(m.month).substr(0, 3);
        break;
      case 'W':
      case 'a': {
        if (!m.month && !m.year) return std::nullopt;
        const auto name = weekday_name(static_cast<uint32_t>(weekday_of(day_number(m), false)));
        out += spec == 'W' ? name : name.substr(0, 3);
        break;
      }
      case 'D':
        out += std::to_string(m.day);
        out += m.day >= 10 && m.day <= 19 ? "th" : m.day % 10 == 1 ? "st" : m.day % 10 == 2 ? "nd" : m.day % 10 == 3 ? "rd" : "th";
        break;
      case 'Y':
        if (m.year > 9999) return std::nullopt;
        padded(m.year, 4);
        break;
      case 'y':
        padded(m.year % 100, 2);
        break;
      case 'm':
        padded(m.month, 2);
        break;
      case 'c':
        out += std::to_string(m.month);
        break;
      case 'd':
        padded(m.day, 2);
        break;
      case 'e':
        out += std::to_string(m.day);
        break;
      case 'f':
        padded(m.micro, 6);
        break;
      case 'H':
        padded(m.hour, 2);
        break;
      case 'h':
      case 'I':
        padded(hour12, 2);
        break;
      case 'i':
        padded(m.minute, 2);
        break;
      case 'j':
        padded(day_number(m) - day_number(Moment{false, m.year, 1, 1, 0, 0, 0, 0}) + 1, 3);
        break;
      case 'k':
        out += std::to_string(m.hour);
        break;
      case 'l':
        out += std::to_string(hour12);
        break;
      case 'p':
        out += meridiem;
        break;
      case 'r':
        std::snprintf(buffer, sizeof buffer, "%02u:%02u:%02u %s", hour12, m.minute, m.second, meridiem);
        out += buffer;
        break;
      case 'S':
      case 's':
        padded(m.second, 2);
        break;
      case 'T':
        std::snprintf(buffer, sizeof buffer, "%02u:%02u:%02u", m.hour, m.minute, m.second);
        out += buffer;
        break;
      case 'U':
      case 'u':
        padded(week(m, spec == 'U' ? week_first_weekday : week_monday_first, &week_year_out), 2);
        break;
      case 'V':
      case 'v':
        padded(week(m, week_year | (spec == 'V' ? week_first_weekday : week_monday_first), &week_year_out), 2);
        break;
      case 'X':
      case 'x':
        week(m, week_year | (spec == 'X' ? week_first_weekday : week_monday_first), &week_year_out);
        padded(week_year_out, 4);
        break;
      case 'w':
        if (!m.month && !m.year) return std::nullopt;
        out += std::to_string(weekday_of(day_number(m), true));
        break;
      default:
        out += spec;
        break;
    }
  }
  return out;
}

uint32_t format_length(std::string_view f) {
  uint32_t size = 0;
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] != '%' || i + 1 == f.size()) {
      ++size;
      continue;
    }
    switch (f[++i]) {
      case 'M':
      case 'W':
        size += 64;
        break;
      case 'D':
      case 'Y':
      case 'x':
      case 'X':
        size += 4;
        break;
      case 'a':
      case 'b':
        size += 32;
        break;
      case 'j':
        size += 3;
        break;
      case 'k':
      case 'H':
        size += 7;
        break;
      case 'r':
        size += 11;
        break;
      case 'T':
        size += 8;
        break;
      case 'f':
        size += 6;
        break;
      case 'U':
      case 'u':
      case 'V':
      case 'v':
      case 'y':
      case 'm':
      case 'd':
      case 'h':
      case 'I':
      case 'i':
      case 'l':
      case 'p':
      case 'S':
      case 's':
      case 'c':
      case 'e':
        size += 2;
        break;
      default:
        ++size;
        break;
    }
  }
  return size;
}

FormatParts format_parts(std::string_view f) {
  FormatParts out;
  for (size_t i = 0; i + 1 < f.size(); ++i) {
    if (f[i] != '%') continue;
    const char spec = f[++i];
    if (std::string_view("aDdbcjMmUuVvWwXxYye").find(spec) != std::string_view::npos) out.date = true;
    if (std::string_view("fHhIiklprSsT").find(spec) != std::string_view::npos) out.time = true;
    if (spec == 'f') out.fraction = true;
  }
  return out;
}

namespace {

// Which of `names` the letters at `at` spell, whatever their case: 1 for
// the first, 0 for none; `at` moves past them.
int word_index(std::string_view s, size_t& at, std::string_view (*name)(uint32_t), uint32_t count, size_t length) {
  size_t end = at;
  while (end < s.size() && std::isalpha(static_cast<unsigned char>(s[end]))) ++end;
  const auto word = s.substr(at, end - at);
  for (uint32_t k = 0; k < count; ++k) {
    auto candidate = name(k);
    if (length) candidate = candidate.substr(0, length);
    if (candidate.size() != word.size()) continue;
    bool same = true;
    for (size_t j = 0; j < word.size() && same; ++j)
      same = std::tolower(static_cast<unsigned char>(word[j])) == std::tolower(static_cast<unsigned char>(candidate[j]));
    if (same) {
      at = end;
      return static_cast<int>(k) + 1;
    }
  }
  return 0;
}

std::string_view month_from_zero(uint32_t k) { return month_name(k + 1); }

}  // namespace

Parsed parse_format(std::string_view s, std::string_view f) {
  Parsed out;
  Moment m;
  size_t v = 0;
  bool usa_time = false;
  uint32_t daypart = 0;
  int64_t yearday = 0, week_number = -1, weekday = 0, strict_year = -1;
  bool sunday_first = false, strict_week = false, strict_year_sunday = false;
  // Up to `n` digits as a number; none is a mismatch (observed: 'x' by '%Y'
  // is 1411 alone, no truncation).
  bool no_digits = false;
  const auto digits = [&](size_t n, size_t* read = nullptr) {
    int64_t value = 0;
    size_t k = 0;
    for (; k < n && v < s.size() && digit(s[v]); ++k, ++v) value = value * 10 + (s[v] - '0');
    if (read) *read = k;
    if (!k) no_digits = true;
    return value;
  };
  while (v < s.size() && space(s[v])) ++v;
  // Reads the text by `fmt`; false where it does not match.
  const std::function<bool(std::string_view)> read = [&](std::string_view fmt) {
    for (size_t p = 0; p < fmt.size() && v < s.size(); ++p) {
      if (space(fmt[p])) {
        while (v < s.size() && space(s[v])) ++v;
        continue;
      }
      if (fmt[p] != '%' || p + 1 == fmt.size()) {
        if (fmt[p] != s[v]) return false;
        ++v;
        continue;
      }
      size_t n = 0;
      switch (fmt[++p]) {
        case 'Y':
        case 'y': {
          const int64_t year = digits(fmt[p] == 'Y' ? 4 : 2, &n);
          m.year = static_cast<uint32_t>(n <= 2 ? (year < 70 ? year + 2000 : year < 100 ? year + 1900 : year) : year);
          break;
        }
        case 'm':
        case 'c':
          m.month = static_cast<uint32_t>(digits(2));
          break;
        case 'M':
        case 'b': {
          const int k = word_index(s, v, month_from_zero, 12, fmt[p] == 'b' ? 3 : 0);
          if (!k) return false;
          m.month = static_cast<uint32_t>(k);
          break;
        }
        case 'd':
        case 'e':
          m.day = static_cast<uint32_t>(digits(2));
          break;
        case 'D':
          m.day = static_cast<uint32_t>(digits(2));
          v = std::min(s.size(), v + 2);
          break;
        case 'h':
        case 'I':
        case 'l':
          usa_time = true;
          m.hour = static_cast<uint32_t>(digits(2));
          break;
        case 'k':
        case 'H':
          m.hour = static_cast<uint32_t>(digits(2));
          break;
        case 'i':
          m.minute = static_cast<uint32_t>(digits(2));
          break;
        case 's':
        case 'S':
          m.second = static_cast<uint32_t>(digits(2));
          break;
        case 'f': {
          int64_t micro = digits(6, &n);
          for (; n < 6; ++n) micro *= 10;
          m.micro = static_cast<uint32_t>(micro);
          break;
        }
        case 'p': {
          if (s.size() - v < 2 || !usa_time) return false;
          const auto two = std::string{static_cast<char>(std::toupper(static_cast<unsigned char>(s[v]))),
                                       static_cast<char>(std::toupper(static_cast<unsigned char>(s[v + 1])))};
          if (two == "PM")
            daypart = 12;
          else if (two != "AM")
            return false;
          v += 2;
          break;
        }
        case 'W':
        case 'a':
          weekday = word_index(s, v, weekday_name, 7, fmt[p] == 'a' ? 3 : 0);
          if (!weekday) return false;
          break;
        case 'w':
          if (v >= s.size() || !digit(s[v])) return false;
          weekday = s[v++] - '0';
          if (weekday >= 7) return false;
          if (!weekday) weekday = 7;
          break;
        case 'j':
          yearday = digits(3);
          break;
        case 'V':
        case 'U':
        case 'v':
        case 'u':
          sunday_first = fmt[p] == 'U' || fmt[p] == 'V';
          strict_week = fmt[p] == 'V' || fmt[p] == 'v';
          week_number = digits(2);
          break;
        case 'X':
        case 'x':
          strict_year_sunday = fmt[p] == 'X';
          strict_year = digits(4);
          break;
        case 'r':
          if (!read("%I:%i:%S %p")) return false;
          break;
        case 'T':
          if (!read("%H:%i:%S")) return false;
          break;
        case '.':
          while (v < s.size() && std::ispunct(static_cast<unsigned char>(s[v]))) ++v;
          break;
        case '@':
          while (v < s.size() && std::isalpha(static_cast<unsigned char>(s[v]))) ++v;
          break;
        case '#':
          while (v < s.size() && digit(s[v])) ++v;
          break;
        default:
          if (fmt[p] != s[v]) return false;
          ++v;
          break;
      }
      if (no_digits) return false;
    }
    return true;
  };
  if (!read(f)) return out;
  if (usa_time) {
    if (m.hour > 12 || m.hour < 1) return out;
    m.hour = m.hour % 12 + daypart;
  }
  constexpr int64_t max_day_number = 3652424;
  if (yearday > 0) {
    const int64_t day = day_number(Moment{false, m.year, 1, 1, 0, 0, 0, 0}) + yearday - 1;
    if (day <= 0 || day > max_day_number) return out;
    const auto date = from_day_number(day);
    m.year = date.year;
    m.month = date.month;
    m.day = date.day;
  }
  if (week_number >= 0 && weekday) {
    // %V and %v need %X and %x; %U and %u go with %Y.
    if ((strict_week && (strict_year < 0 || strict_year_sunday != sunday_first)) || (!strict_week && strict_year >= 0)) return out;
    const auto year = static_cast<uint32_t>(strict_week ? strict_year : m.year);
    int64_t day = day_number(Moment{false, year, 1, 1, 0, 0, 0, 0});
    const int64_t first = weekday_of(day, sunday_first);
    if (sunday_first)
      day += (first == 0 ? 0 : 7) - first + (week_number - 1) * 7 + weekday % 7;
    else
      day += (first <= 3 ? 0 : 7) - first + (week_number - 1) * 7 + (weekday - 1);
    if (day <= 0 || day > max_day_number) return out;
    const auto date = from_day_number(day);
    m.year = date.year;
    m.month = date.month;
    m.day = date.day;
  }
  if (m.month > 12 || m.day > 31 || m.hour > 23 || m.minute > 59 || m.second > 59) return out;
  for (size_t k = v; k < s.size(); ++k) {
    if (!space(s[k])) {
      out.truncated = true;
      break;
    }
  }
  out.moment = m;
  return out;
}

std::string shifted(std::string_view datetime, int fsp, int64_t seconds) {
  const auto m = parse_text(datetime, Kind::datetime);
  return text(from_unix_us(to_unix_us(m) + seconds * 1000000), Kind::datetime, fsp);
}

int64_t ordinal(const Moment& m, Kind kind) {
  const int64_t seconds = static_cast<int64_t>(m.hour) * 3600 + m.minute * 60 + m.second;
  if (kind == Kind::time) {
    const int64_t v = seconds * 1000000 + m.micro;
    return m.negative ? -v : v;
  }
  const int64_t day = (static_cast<int64_t>(m.year) * 13 + m.month) * 32 + m.day;
  return (day * 86400 + (kind == Kind::date ? 0 : seconds)) * 1000000 + (kind == Kind::date ? 0 : m.micro);
}

}  // namespace kvasir::exec::temporal
