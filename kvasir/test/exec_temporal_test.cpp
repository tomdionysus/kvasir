//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "kvasir/exec/temporal.h"

namespace kvasir::exec::temporal {
namespace {

// The text a string reads as, or "-" for none.
std::string datetime(std::string_view s, int fsp = 0) {
  const auto r = read_datetime(s, fsp);
  return r.moment ? text(*r.moment, Kind::datetime, fsp) : "-";
}
std::string time(std::string_view s, int fsp = 0) {
  const auto r = read_time(s, fsp);
  return r.moment ? text(*r.moment, Kind::time, fsp) : "-";
}

// MySQL's reading of dates and times (TODO/2026-11-23-temporal-notes.md):
// any punctuation as a delimiter, lengths without one, two-digit years, a
// fraction rounded half up and carried, zero and impossible dates refused.
TEST(Temporal, DatesAndDatetimes) {
  EXPECT_EQ(datetime("2024-02-29"), "2024-02-29 00:00:00");
  EXPECT_EQ(datetime("2024/2/3 4.5.6"), "2024-02-03 04:05:06");
  EXPECT_EQ(datetime("2024-02-03T04:05"), "2024-02-03 04:05:00");
  EXPECT_EQ(datetime("20240203"), "2024-02-03 00:00:00");
  EXPECT_EQ(datetime("240203"), "2024-02-03 00:00:00");
  EXPECT_EQ(datetime("700101"), "1970-01-01 00:00:00");
  EXPECT_EQ(datetime("20240203040506.123456", 3), "2024-02-03 04:05:06.123");
  EXPECT_EQ(datetime("240203040506"), "2024-02-03 04:05:06");
  EXPECT_EQ(datetime("2024-12-31 23:59:59.5"), "2025-01-01 00:00:00");
  EXPECT_EQ(datetime("2024-02-29 23:59:59.9999999", 6), "2024-03-01 00:00:00.000000");
  for (const auto* bad : {"2024-02-30", "0000-00-00", "2024-00-01", "2024-01-00", "2024-13-01", "abc", "", "2024-01-01 24:00:00", "1234567"})
    EXPECT_EQ(datetime(bad), "-") << bad;
  const auto r = read_datetime(" 2024-02-03 04:05:06 xyz", 0);
  EXPECT_TRUE(r.truncated);
  EXPECT_EQ(r.superfluous, (std::vector<size_t>{0, 20}));
}

// TIME: hh:mm:ss, days before a space, digits from the right, a sign; past
// 838:59:59 a string is clamped and a number refused; a date and time
// gives its time.
TEST(Temporal, Times) {
  EXPECT_EQ(time("12:34:56"), "12:34:56");
  EXPECT_EQ(time("12:34"), "12:34:00");
  EXPECT_EQ(time("-12:00"), "-12:00:00");
  EXPECT_EQ(time("1 02:03:04"), "26:03:04");
  EXPECT_EQ(time("2 3"), "51:00:00");
  EXPECT_EQ(time("0203"), "00:02:03");
  EXPECT_EQ(time("3"), "00:00:03");
  EXPECT_EQ(time("1234567"), "-");
  EXPECT_EQ(time("1234559"), "123:45:59");
  EXPECT_EQ(time("12:00:00.25", 1), "12:00:00.3");
  EXPECT_EQ(time("2024-02-03 04:05:06"), "04:05:06");
  EXPECT_EQ(time("12:60:00"), "-");
  EXPECT_EQ(time("abc"), "-");
  const auto clamped = read_time("839:00:00", 0);
  EXPECT_TRUE(clamped.clamped);
  EXPECT_EQ(text(*clamped.moment, Kind::time, 0), "838:59:59");
  bool over = false;
  EXPECT_FALSE(time_from_number(8390000, 0, false, 0, &over));
  EXPECT_EQ(text(*time_from_number(125, 500000, true, 0, &over), Kind::time, 0), "-00:01:26");
  EXPECT_EQ(text(*time_from_number(20240203040506, 0, false, 0, &over), Kind::time, 0), "04:05:06");
  EXPECT_FALSE(time_from_number(1261, 0, false, 0, &over));
  // Rounded past the limit, a number's TIME is clamped too.
  EXPECT_EQ(text(*time_from_number(8385959, 500000, false, 0, &over), Kind::time, 0), "838:59:59");
  EXPECT_TRUE(over);
}

// Numbers as dates: MySQL's ranges for YYMMDD, YYYYMMDD and their times.
TEST(Temporal, Numbers) {
  const auto date = [](uint64_t n, uint32_t micro = 0, int fsp = 0) {
    const auto m = datetime_from_number(n, micro, false, fsp);
    return m ? text(*m, Kind::datetime, fsp) : "-";
  };
  EXPECT_EQ(date(240203), "2024-02-03 00:00:00");
  EXPECT_EQ(date(991231), "1999-12-31 00:00:00");
  EXPECT_EQ(date(20240203), "2024-02-03 00:00:00");
  EXPECT_EQ(date(240203040506), "2024-02-03 04:05:06");
  EXPECT_EQ(date(991231235959), "1999-12-31 23:59:59");
  EXPECT_EQ(date(20240203040506, 999999, 3), "2024-02-03 04:05:07.000");
  for (const uint64_t bad : {0ULL, 1ULL, 691232ULL, 9991231ULL, 100000000ULL, 691232000000ULL, 9991231235959ULL, 99991232000000ULL, 100000000000000ULL})
    EXPECT_EQ(date(bad), "-") << bad;
  EXPECT_FALSE(datetime_from_number(20240203, 0, true, 0));
}

// What a value is as text, as a number, and in order.
TEST(Temporal, TextNumbersAndOrder) {
  const Moment m{false, 2024, 2, 3, 4, 5, 6, 789000};
  EXPECT_EQ(text(m, Kind::date, 0), "2024-02-03");
  EXPECT_EQ(text(m, Kind::datetime, 3), "2024-02-03 04:05:06.789");
  EXPECT_EQ(number_text(m, Kind::date, 0), "20240203");
  EXPECT_EQ(number_text(m, Kind::datetime, 2), "20240203040506.78");
  EXPECT_EQ(number_text(Moment{true, 0, 0, 0, 12, 0, 0, 0}, Kind::time, 0), "-120000");
  EXPECT_EQ(parse_text("2024-02-03 04:05:06.789", Kind::datetime), m);
  EXPECT_EQ(parse_text("-838:59:59.5", Kind::time), (Moment{true, 0, 0, 0, 838, 59, 59, 500000}));
  EXPECT_LT(ordinal(parse_text("2024-02-03", Kind::date), Kind::date), ordinal(m, Kind::datetime));
  EXPECT_LT(ordinal(parse_text("-01:00:00", Kind::time), Kind::time), ordinal(parse_text("00:00:00", Kind::time), Kind::time));
  auto r = m;
  round(r, Kind::date, 0);
  EXPECT_EQ(text(r, Kind::date, 0), "2024-02-03");
  Moment t{false, 0, 0, 0, 838, 59, 59, 999999};
  round(t, Kind::time, 0);
  EXPECT_EQ(text(t, Kind::time, 0), "838:59:59");
  // The Unix epoch's microseconds, either side of it.
  EXPECT_EQ(text(from_unix_us(1709210096789000), Kind::datetime, 6), "2024-02-29 12:34:56.789000");
  EXPECT_EQ(text(from_unix_us(-1), Kind::datetime, 6), "1969-12-31 23:59:59.999999");
  for (const int64_t us : {int64_t{0}, int64_t{-1}, int64_t{1709210096789000}, int64_t{-62135596800000000}, int64_t{253402300799999999}})
    EXPECT_EQ(to_unix_us(from_unix_us(us)), us) << us;
  EXPECT_EQ(shifted("2024-01-01 00:00:00", 0, 36000), "2024-01-01 10:00:00");
  EXPECT_EQ(shifted("2024-03-01 00:30:00.5", 1, -3600), "2024-02-29 23:30:00.5");
  EXPECT_TRUE(valid_date(2024, 2, 29));
  EXPECT_FALSE(valid_date(2023, 2, 29));
  EXPECT_FALSE(valid_date(1900, 2, 29));
  EXPECT_TRUE(valid_date(2000, 2, 29));
}

}  // namespace
}  // namespace kvasir::exec::temporal
