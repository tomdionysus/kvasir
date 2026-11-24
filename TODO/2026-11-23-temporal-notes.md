# Temporal types: what MySQL 8.4.13 was observed to do (2026-11-23)

The reference server's answers, gathered before T1b, the facts the temporal
module is written to. Each goes into the differential gate as it is built.

## Literals and their metadata

- `DATE '2024-02-29'`: DATE, binary collation, length 10, decimals 0, flags
  NOT_NULL BINARY (no NUM). `TIME '12:34:56'`: TIME, length 10 (8 shown).
  `TIMESTAMP '2024-02-29 12:34:56'`: DATETIME, length 19. With a fraction the
  type takes its digits: `TIME '12:34:56.789'` TIME length 14 decimals 3;
  `TIMESTAMP '... 12:34:56.5'` DATETIME length 21 decimals 1.
- An invalid literal is an error: `DATE '2024-02-30'` is 1525 "Incorrect
  DATE value: '2024-02-30'".

## CAST to DATE, TIME, DATETIME

- Types as the literals', but nullable (flags BINARY only); `DATETIME(2)` is
  length 22 decimals 2; `TIME(3)` length 14 decimals 3.
- Strings: delimiters any punctuation ('2024/02/03'), single digits
  ('2024-2-3', '4:5:6'), no delimiters by length ('20240203', '240203',
  '20240203040506'), 'T' between date and time, a missing time is midnight,
  a missing second 0. Two-digit years: 70-99 are 19xx, 00-69 20xx.
- Invalid or zero dates are NULL with warning 1292 "Incorrect datetime
  value: '...'" ('2024-13-01', 'abc', '', '0000-00-00', '2024-00-01',
  '2024-01-00' all NULL).
- Fractions round, and rounding carries: '2024-02-29 23:59:59.9999999' as
  DATETIME(6) is 2024-03-01 00:00:00.000000; '2024-12-31 23:59:59.5' as
  DATETIME is 2025-01-01 00:00:00.
- Trailing text: '2024-02-03 04:05:06 xyz' is 2024-02-03 04:05:06 with 1292
  "Truncated incorrect datetime value" and 4096 "Delimiter ' ' in position
  19 in datetime value '...' at row 1 is superfluous and is deprecated.
  Please remove."; a leading or trailing space alone gives only the 4096.
- TIME strings: '838:59:59' the most; '839:00:00' clamps to 838:59:59 with
  1292 "Truncated incorrect time value: '839:00:00'"; '-12:00' is
  -12:00:00; '1 02:03:04' is 26:03:04 (days); '0203' is 00:02:03; '3' is
  00:00:03; '12:60:00' is NULL with 1292 "Truncated incorrect time value".
- Numbers: 20240203 and 240203 as DATE; 20240203040506 as DATETIME; 203 as
  TIME 00:02:03; -10000 as TIME -01:00:00; 12.5 as TIME 00:00:13 (rounded);
  20240203.5 as DATETIME 2024-02-03 00:00:00; 1 as DATE NULL with 1292
  "Incorrect datetime value: '1'".
- Between temporals: DATE as DATETIME is midnight; DATETIME as DATE drops
  the time; DATETIME '... 04:05:06.7' as TIME is 04:05:07 (rounded).

## In expressions

- `DATE '2024-01-01' + 0` is LONGLONG 20240101 (length 10); `TIME + 0`
  LONGLONG 100000 (length 9); `TIMESTAMP + 0` LONGLONG 20240101100000
  (length 16); with a fraction NEWDECIMAL 20240101100000.5.
- Comparisons: DATE < DATE as dates; `DATE '2024-01-01' = '2024-01-01'` is
  1; `TIMESTAMP '2024-01-01 00:00:00' = DATE '2024-01-01'` is 1; but
  `TIME '10:00:00' > '9:00:00'` is 0 (compared as strings).
- `CONCAT(DATE '2024-01-01')` is VAR_STRING length 40 '2024-01-01';
  `LENGTH(TIME '01:02:03')` 8.
- Mixed in flow functions: `COALESCE(DATE, DATETIME)` and `IF(1, DATE,
  TIME)` report DATETIME with utf8mb4_0900_ai_ci, length 76, NOT_NULL only,
  and show '2024-01-01 00:00:00'; `GREATEST(DATE, DATE)` reports DATE with
  utf8mb4_0900_ai_ci, length 40.

## Columns (T2)

- Column metadata: DATE length 10, flags BINARY (and NOT_NULL as defined);
  TIME(3) length 14 (13 shown) decimals 3; DATETIME(6) length 26 decimals
  6; TIMESTAMP field type TIMESTAMP, length 19; YEAR field type YEAR,
  length 4, flags UNSIGNED ZEROFILL NUM.
- Stored as given: '12:34:56.7891' in TIME(3) is 12:34:56.789; a DATETIME(6)
  of '...56.1234567' is .123457 (rounded); numbers by the CAST rules
  (20240301, 123456); '24-3-2' is 2024-03-02; '-1 02:00' in TIME(3) is
  -26:00:00.000; YEAR 2024 or '1999'; YEAR 24 is 2024.
- Strict (the default sql_mode): an invalid date, a zero date
  ('0000-00-00', with NO_ZERO_DATE), a TIME past 838:59:59 are 1292
  "Incorrect date value: '2024-02-30' for column 'd' at row 1" (date, time,
  datetime by the column's type); YEAR 1900 is 1264 "Out of range value for
  column 'y' at row 1"; a TIMESTAMP before 1970-01-01 00:00:01 UTC is 1292
  "Incorrect datetime value: ... for column 'ts'"; a DATETIME string into a
  DATE keeps the date with Note 1292 "Incorrect date value: '2024-02-03
  04:05:06' for column 'd' at row 1".
- Not strict (sql_mode ''): each of those is stored as the zero value
  (0000-00-00, 838:59:59.000 clamped, YEAR 0000) with Warning 1264 "Out of
  range value for column 'd' at row 1".
- In expressions: d + 0 LONGLONG 20240229 (length 10, nullable: BINARY
  NUM); t + 0 of TIME(3) NEWDECIMAL 123456.789 (length 13, 3 decimals);
  dt + 0 of DATETIME(6) NEWDECIMAL length 23 decimals 6; y + 0 LONGLONG
  UNSIGNED length 5; d = '2024-02-29' and y = 2024 compare as values.
- information_schema.COLUMNS: DATA_TYPE date, time, datetime, timestamp,
  year; COLUMN_TYPE time(3), datetime(6), timestamp, year;
  DATETIME_PRECISION the fsp for TIME, DATETIME and TIMESTAMP (0 when none
  is written), NULL for DATE and YEAR; COLUMN_DEFAULT CURRENT_TIMESTAMP and
  EXTRA "DEFAULT_GENERATED on update CURRENT_TIMESTAMP" for DATETIME DEFAULT
  CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP; a DATE default '2000-01-01'.

## CURRENT_TIMESTAMP defaults and ON UPDATE (T2b)

- `a DATETIME DEFAULT CURRENT_TIMESTAMP`: flags BINARY; COLUMN_DEFAULT
  CURRENT_TIMESTAMP, EXTRA DEFAULT_GENERATED. `b TIMESTAMP(3) NOT NULL
  DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3)`: flags
  NOT_NULL BINARY TIMESTAMP ON_UPDATE_NOW (no NO_DEFAULT_VALUE);
  COLUMN_DEFAULT CURRENT_TIMESTAMP(3), EXTRA "DEFAULT_GENERATED on update
  CURRENT_TIMESTAMP(3)". `c DATETIME ON UPDATE NOW()`: flags BINARY only;
  COLUMN_DEFAULT NULL, EXTRA "on update CURRENT_TIMESTAMP".
- NOW(), LOCALTIME and LOCALTIMESTAMP serve as CURRENT_TIMESTAMP, with
  the column's fraction digits.
- An omitted column takes the statement's time; an UPDATE that changes a
  row sets its ON UPDATE columns not SET to the statement's time; one that
  changes nothing leaves them (ROW_COUNT 0).
- Refused: DATETIME(3) DEFAULT CURRENT_TIMESTAMP (fraction digits differ)
  and DATE DEFAULT CURRENT_TIMESTAMP are 1067 "Invalid default value for
  'a'"; INT ON UPDATE CURRENT_TIMESTAMP and DATETIME ON UPDATE
  CURRENT_TIMESTAMP(2) are 1294 "Invalid ON UPDATE clause for 'a' column".
- `DEFAULT (CURRENT_DATE)` (an expression default) is MySQL 8's
  generated default: COLUMN_DEFAULT curdate(), EXTRA DEFAULT_GENERATED.

## INTERVAL arithmetic (T4b)

- DATE_ADD, DATE_SUB, ADDDATE, SUBDATE, `+ INTERVAL`, `- INTERVAL`,
  `INTERVAL n unit + d`, TIMESTAMPADD: the result's type follows the
  operand and the unit: a DATE with a day unit or longer is a DATE; a DATE
  with a time unit, or a DATETIME, is a DATETIME; a TIME with a time unit
  is a TIME; a string operand gives STRING (field type 254),
  utf8mb4_0900_ai_ci, length 116, decimals 31, nullable, its text a date
  ('2024-02-01') or a datetime ('2024-01-01 11:30:00') as the sum is, with
  6 fraction digits once it has a fraction ('2024-01-01 00:00:01.500000').
- Months clamp the day: '2024-01-31' + 1 MONTH is 2024-02-29; 2024-02-29 +
  1 YEAR is 2025-02-28. ADDDATE(d, n) adds n days. 1.5 DAY rounds to 2.
  Compound units read their string: '1:30' HOUR_MINUTE is 1h30m.
- NULL in, NULL out; 'abc' is NULL with 1292 "Incorrect datetime value:
  'abc'"; past 9999-12-31 NULL with 1441 "Datetime function: datetime
  field overflow".
- TIMESTAMPDIFF: LONGLONG length 21; MONTH between 2024-01-31 and
  2024-02-29 is 0 (whole months only); SECOND over a day 86400.
- EXTRACT: YEAR_MONTH of 2024-02-29 is 202402 (LONGLONG length 7); HOUR of
  TIME '838:00:00' 838 (length 4).
- The amount (observed 2026-11-23, second probe): a simple unit's is an
  integer, a number rounded (1.5 DAY is 2, -1.5 is -2), a string's leading
  integer with 1292 "Truncated incorrect INTEGER value: '1.5'"; SECOND
  keeps a fraction, adding its digits to the result's (1.25 SECOND gives
  DATETIME(2), '1.5' gives 6); a microsecond unit gives 6. A compound
  unit's amount is read as text: optional spaces and '-', then digit
  groups split by anything else; fewer groups fill the last fields ('5'
  HOUR_MINUTE is 5 minutes, 1 DAY_HOUR one hour), a microsecond group is
  scaled by its digits ('1.5' SECOND_MICROSECOND is 1.5 s, '5' is 0.5 s);
  more groups than fields is NULL with 1441 "Datetime function:
  date_add_interval field overflow"; no digits at all is zero.
- Days and time are counted from the first of the month; a day number of
  365 or under is 0000-00-00 (0001-01-01 - 1 DAY); YEAR -1 of 0001-01-01 is
  0000-01-01. A TIME moved past 838:59:59 is NULL with 1441 "Datetime
  function: time field overflow"; a TIME moved by a day or longer is a
  DATETIME on the statement's date; a string TIME ('10:00:00') is no
  datetime (1292, NULL). A YEAR column's operand gives STRING and NULL.
- TIMESTAMPDIFF months: the months between, one fewer when the later's day
  and time come before the earlier's; backwards is negative.
- EXTRACT: YEAR, QUARTER, MONTH, WEEK (mode 0), DAY and YEAR_MONTH read a
  date (a TIME on the statement's date); the rest read a time keeping a
  date and time's day (DAY_HOUR of '2024-02-29 10:00' is 2910, of TIME
  '30:00:00' is 30), signed. Lengths: YEAR 5, QUARTER 2, MONTH 3, WEEK 3,
  DAY 3, HOUR 4, MINUTE 3, SECOND 3, MICROSECOND 7, YEAR_MONTH 7, DAY_HOUR
  9, DAY_MINUTE 11, DAY_SECOND 13, DAY_MICROSECOND 20, HOUR_MINUTE 6,
  HOUR_SECOND 8, HOUR_MICROSECOND 14, MINUTE_SECOND 5, MINUTE_MICROSECOND
  11, SECOND_MICROSECOND 9.

## The clock

- NOW() and its kin take the statement's time from an injected wall clock:
  Dannan's dannan::contract::WallClock (milliseconds; microseconds asked of
  Dannan on 2026-11-23).

## DATE_FORMAT, TIME_FORMAT and WEEK (T4c)

- DATE_FORMAT and TIME_FORMAT: VAR_STRING, nullable, length the format's
  characters by MySQL's sizes (%M %W 64, %a %b 32, %H %k 7, %r 11, %T 8, %f
  6, %D %Y %X %x 4, %j 3, two-digit ones 2, others and literal characters
  1) times the result's widest character; a format not a constant string
  ten times its own length; a NULL format 0. An empty format is NULL; an
  unknown specifier is its letter, a trailing % itself. DATE_FORMAT reads
  a date (a TIME on the statement's date); TIME_FORMAT reads a time (a
  date's time), gives NULL for %W %a %D %j %U %u %V %v %X %x %w %M %b,
  zeros for %Y %y %m %c %d %e, hours past 23 in full, and a negative
  TIME's sign once at the front.
- WEEK (LONGLONG length 3; one or two arguments by the grammar, a NULL
  mode is 0, the mode's low three bits), YEARWEEK (length 7, the week's
  year), WEEKOFYEAR (mode 3): MySQL's calc_week.

## STR_TO_DATE (T4c, probed)

- Typed by a constant format's specifiers: date ones only DATE, time ones
  only TIME, both DATETIME; %f gives 6 fraction digits. A NULL format, or
  one not constant, gives DATETIME(6). Always nullable.
- A result MySQL's strict mode refuses (a zero month or day, an invalid
  date: '2024' by '%Y', '2024-02-30') is NULL with 1411 "Incorrect
  datetime value: '2024' for function str_to_date". Text left after the
  format is kept with 1292 "Truncated incorrect date value:
  '2024-02-29xyz'". Leading spaces are skipped. %h %p, %r, %b, %W %D %M,
  %X %V %w and %Y %j all read.

## UNIX_TIMESTAMP and FROM_UNIXTIME (T4c, probed)

- UNIX_TIMESTAMP(x): LONGLONG length 21 when x has no fraction digits, else
  NEWDECIMAL with x's digits (a constant string's own: '...56.5' gives 1,
  length 14; DATETIME(3) gives 3, length 16); a constant string that is no
  datetime ('x') NEWDECIMAL(19, 6) and 0.000000 with 1292. NOT_NULL for a
  non-NULL constant string or number (a CAST is nullable). Before 1970 or
  past 3001-01-18 23:59:59 UTC: 0 (3001-01-19 03:14:07 is 0 too). A DATE is its midnight; a TIME takes
  the statement's date. In the session's zone.
- FROM_UNIXTIME(n): DATETIME, n's fraction digits (a string 6, a DOUBLE 6,
  rounded: .123456789 gives .123457), nullable; negative or past
  32536771199 (3001-01-18 23:59:59 UTC) NULL, no warning. With a format:
  VAR_STRING by DATE_FORMAT's lengths.

## ADDTIME, TIMEDIFF and the small time functions (T4c, probed)

- ADDTIME(a, b), SUBTIME: a DATE, DATETIME or TIMESTAMP gives DATETIME, a
  TIME gives TIME, anything else STRING (length 29 characters) holding a
  date and time or a time as a's string reads; b is read as a time (a
  date's or datetime's time alone; '1 01:00:00' is 25 hours); fraction
  digits the wider of the two (a constant string's written ones). A TIME
  past 838:59:59 is clamped with 1292 "Truncated incorrect time value:
  '839:00:00'"; 'x' is NULL with 1292 "Truncated incorrect time value".
- TIMEDIFF(a, b): TIME; two dates and times, or two times, else NULL (a
  TIMESTAMP and a TIME is NULL); clamped as above ('2160:00:00').
- MAKEDATE(y, d): DATE, NULL for d under 1, years 0-69 2000s and 70-99
  1900s. MAKETIME(h, m, s): TIME with s's digits, NULL for m or s out of
  range, hours past 838 clamped with 1292 naming '900:00:00'.
  SEC_TO_TIME(n): TIME with n's digits, clamped with 1292 naming n as
  written. TIME_TO_SEC: LONGLONG length 10, the fraction dropped.
  FROM_DAYS: DATE, NOT_NULL for a constant, 365 and under 0000-00-00.
  TO_SECONDS: LONGLONG length 21.

## The binary protocol (T5, observed 2026-11-24)

Through mysql-connector-python 9.x's prepared statements
(`build/connectors/py_temporal.py`, answers beside it):
- A bound DATE is a DATE (a table made from it: `date`); a bound DATETIME
  is DATETIME(6) whatever its fraction (`datetime(6)`, CONCAT shows
  '2024-02-29 12:34:56.000000'); a bound TIME is TIME(6). `? + 0` of a
  DATE is LONGLONG 20240229, of a DATETIME NEWDECIMAL with 6 digits.
- A TIME sent past 838:59:59 (40 days) reads as 838:59:59.000000, with no
  warning; DATE_ADD of it is NULL.
- Bound dates compare with columns and insert into them as values.
- Binary rows: DATE, DATETIME, TIMESTAMP and TIME columns as the manual's
  Binary Protocol Value (length 0, 4, 7 or 11; TIME 0, 8 or 12). The
  connector itself misreads a YEAR and a negative TIME, so their bytes
  are taken from the manual and checked with Connector/C++.
