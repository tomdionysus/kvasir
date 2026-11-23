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

## The clock

- NOW() and its kin take the statement's time from an injected wall clock:
  Dannan's dannan::contract::WallClock (milliseconds; microseconds asked of
  Dannan on 2026-11-23).
