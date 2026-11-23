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

## The clock

- NOW() and its kin take the statement's time from an injected wall clock:
  Dannan's dannan::contract::WallClock (milliseconds; microseconds asked of
  Dannan on 2026-11-23).
