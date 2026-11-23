//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Dates and times as MySQL 8.4 reads, checks, rounds and writes them (the
// manual's "Date and Time Data Types" and "Date and Time Literals", as
// observed on 8.4.13: TODO/2026-11-23-temporal-notes.md). A DATE, TIME or
// DATETIME is carried as its text in MySQL's own form, and read back here
// when it must be compared or counted.
namespace kvasir::exec::temporal {

enum class Kind : uint8_t { date, time, datetime };

// The fields of a DATE (no time), a TIME (hours up to 838, a sign) or a
// DATETIME.
struct Moment {
  bool negative = false;
  uint32_t year = 0;
  uint32_t month = 0;
  uint32_t day = 0;
  uint32_t hour = 0;
  uint32_t minute = 0;
  uint32_t second = 0;
  uint32_t micro = 0;
  friend bool operator==(const Moment&, const Moment&) = default;
};

// What reading a string gave: the moment, or nothing for a value MySQL does
// not take; and what it warns of.
struct Read {
  std::optional<Moment> moment;
  // Text past the value (1292 "Truncated incorrect ... value").
  bool truncated = false;
  // A TIME past 838:59:59, clamped to it (1292).
  bool clamped = false;
  // A date read with a time after it (a DATETIME, not a DATE).
  bool has_time = false;
  // Positions of delimiters MySQL calls superfluous (4096): a space before
  // or after the value.
  std::vector<size_t> superfluous;
};

// A string as a DATE or DATETIME: delimited by any punctuation or by
// length alone (YYMMDD, YYYYMMDD, YYMMDDhhmmss, YYYYMMDDhhmmss), a 'T' or
// space before the time, two-digit years 70-99 as 19xx and 00-69 as 20xx;
// a zero or impossible date is nothing. The fraction rounded to `fsp`
// digits, carrying into the date.
Read read_datetime(std::string_view text, int fsp);

// A string as a TIME: [-][D ]hh:mm:ss[.f], hh:mm, D hh, or digits read from
// the right (hhmmss, mmss, ss); a date and time gives its time. Past
// 838:59:59 it is clamped; a minute or second past 59 is nothing.
Read read_time(std::string_view text, int fsp);

// A number's integer part, and its fraction in microseconds, as a DATE or
// DATETIME (YYMMDD, YYYYMMDD, YYMMDDhhmmss, YYYYMMDDhhmmss) or as a TIME
// (hhmmss); nothing for a value MySQL does not take.
std::optional<Moment> datetime_from_number(uint64_t integer, uint32_t micro, bool negative, int fsp);
std::optional<Moment> time_from_number(uint64_t integer, uint32_t micro, bool negative, int fsp, bool* clamped);

// A number's text ([-]digits[.digits]) as a DATE or DATETIME, or as a
// TIME: the integer part by the rules above, the fraction as microseconds.
std::optional<Moment> from_number(std::string_view text, Kind kind, int fsp);

// Whether a year, month and day name a real date (no zero part).
bool valid_date(uint32_t year, uint32_t month, uint32_t day);

// A moment made `kind` with `fsp` fraction digits: a DATE's time dropped,
// a fraction rounded half up, carrying (a TIME stops at 838:59:59).
void round(Moment& m, Kind kind, int fsp);

// MySQL's text: 2024-02-29, -12:00:00.5, 2024-02-29 12:34:56.789.
std::string text(const Moment& m, Kind kind, int fsp);

// The moment a value of `kind` carries as text (as `text` writes it).
Moment parse_text(std::string_view text, Kind kind);

// The number a value is in a numeric context: 20240229, 123456,
// 20240229123456, with its fraction's digits after the point.
std::string number_text(const Moment& m, Kind kind, int fsp);

// Microseconds since the Unix epoch as the date and time they are, and
// back.
Moment from_unix_us(int64_t us);
int64_t to_unix_us(const Moment& m);

// A date's day number, as TO_DAYS counts (0001-01-01 is 366); and the date
// of a day number, MySQL's zero date for 365 and under.
int64_t day_number(const Moment& m);
Moment from_day_number(int64_t day);

// The days in a month of a year.
uint32_t days_in_month(uint32_t year, uint32_t month);

// A DATETIME's text moved by `seconds` (a TIMESTAMP between UTC and a
// session's zone).
std::string shifted(std::string_view datetime, int fsp, int64_t seconds);

// A sortable key: earlier is less. A DATE is a DATETIME at midnight.
int64_t ordinal(const Moment& m, Kind kind);

}  // namespace kvasir::exec::temporal
