# Set operations: what MySQL 8.4.13 was observed to do (2026-11-24)

Probed on the reference server (`build/mysql84/probe.sh`), for UNION,
UNION ALL, INTERSECT and EXCEPT.

## Results and their columns

- UNION removes duplicate rows; UNION ALL keeps them; INTERSECT and EXCEPT
  are there too (`SELECT 1 INTERSECT SELECT 1` is 1, `SELECT 1 EXCEPT
  SELECT 2` is 1).
- A column's name is the first SELECT's.
- A column's type is the types of every SELECT's column joined, as a
  temporary table's field: `1 UNION 2` is LONGLONG length 2, flags
  NOT_NULL NUM (no BINARY); `1, 'x' UNION 2.5, 'yy'` is NEWDECIMAL(4,1)
  (1 shows as 1.0) and VAR_STRING length 8; `'abc' UNION 12345678901` is
  VAR_STRING length 48 (11 characters); `1 UNION NULL` is LONGLONG with no
  NOT_NULL; `NULL UNION NULL` is VAR_STRING, binary, length 0, BINARY;
  `DATE UNION TIMESTAMP` is DATETIME, flags NOT_NULL BINARY
  NO_DEFAULT_VALUE, the DATE shown as 2024-01-01 00:00:00.
- A different number of columns is 1222 (21000) "The used SELECT statements
  have a different number of columns".

## Order and limits

- `(SELECT 3 a) UNION (SELECT 1) ORDER BY a DESC LIMIT 2`: the ORDER BY and
  LIMIT after the last SELECT apply to the whole result; ORDER BY 1 by
  position; a name not among the result's columns is 1054 "Unknown column
  'b' in 'order clause'".

## Precedence and the forms

- INTERSECT binds tighter than UNION and EXCEPT, which go left to right:
  `1 UNION 2 INTERSECT 3` is 1; `1 UNION ALL 1 INTERSECT ALL 1` is 1, 1;
  `1 UNION ALL 1 EXCEPT ALL 1` is 1; `1 UNION ALL 1 EXCEPT 2` is 1 (a
  DISTINCT operation dedupes all it has); `1 UNION ALL 1 UNION 1` is 1;
  `1 UNION 1 UNION ALL 1` is 1, 1. UNION DISTINCT is UNION.
- A SELECT with its own ORDER BY or LIMIT must be in parentheses
  (`SELECT 3 a LIMIT 1 UNION SELECT 1` is 1064 near 'UNION SELECT 1');
  inside them they apply to that part: `(SELECT 3 a ORDER BY a LIMIT 1)
  UNION ALL (SELECT 1 UNION SELECT 2 ORDER BY 1 DESC LIMIT 1)` is 3, 2.
  Parentheses nest: `((SELECT 1 a)) UNION ((SELECT 2))`.
- The whole result's ORDER BY takes expressions over its columns (`ORDER
  BY a + 1`); LIMIT 1, 1 skips one.
- Duplicates are judged by each column's collation: `'a' UNION 'A'` is
  'a' alone (utf8mb4_0900_ai_ci). Two explicit collations are 1267 "Illegal
  mix of collations (utf8mb4_bin,EXPLICIT) and
  (utf8mb4_0900_ai_ci,EXPLICIT) for operation 'UNION'".
- Rows come in the order they were first produced (no ORDER BY).

## The joined type (the full matrix is build/union-matrix.txt, 399 pairs)

Each result column is a temporary table's field over every part's column:
- Integers (TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT, YEAR with another
  integer, an integer literal or expression as LONGLONG): the widest field
  type; YEAR with YEAR stays YEAR (UNSIGNED ZEROFILL NUM). Length the
  largest of each part's precision plus one if signed (literal 1 is 2, a
  TINYINT column 4, INT 11, BIGINT 20, BIGINT UNSIGNED 20, INT UNSIGNED 10,
  YEAR 4), not recomputed for the result's sign. UNSIGNED only when every
  part is; a signed part beside the widest being BIGINT UNSIGNED makes
  NEWDECIMAL(20,0) (length 21).
- DECIMAL with integers or DECIMAL: NEWDECIMAL, integer digits the most
  any part has (an integer's precision), scale the most; length precision
  plus a point if scaled plus one. 1 UNION 2.25 shows 1.00.
- DOUBLE with any number: DOUBLE; FLOAT with TINYINT, SMALLINT, BIGINT,
  BIGINT UNSIGNED, YEAR, FLOAT or an integer literal stays FLOAT, with INT,
  DECIMAL, DOUBLE or a decimal literal DOUBLE. Both length 23, decimals 31.
- Temporal: DATE with DATE is DATE; TIME with TIME TIME; any other two
  temporals DATETIME with the most fraction digits (DATE with TIMESTAMP
  DATETIME(0)); TIMESTAMP with TIMESTAMP TIMESTAMP. A DATE in a DATETIME(1)
  shows 2024-01-01 00:00:00.0.
- A string with anything, or a temporal with a number, or with YEAR: a
  string. CHAR (STRING) when every string part is CHAR and there is one,
  else VAR_STRING. Its characters are the most any part takes as text: a
  string's own, a number's display (TINYINT 4, INT 11, BIGINT 20, DECIMAL
  its length, DOUBLE 22, FLOAT 12, integer literal precision plus sign),
  a temporal's length (DATE 10, TIME(2) 13, DATETIME(3) 23, TIMESTAMP 19),
  YEAR 4; length that times 4. A binary string part makes it binary, its
  length in bytes (a utf8mb4 string's bytes). A TEXT part makes BLOB of
  its bytes times 4 (TINYTEXT 4080, TEXT 1048560, MEDIUMTEXT 268435440,
  LONGTEXT 4294967295); a BLOB part BLOB BINARY of its bytes (255, 65535).
  String results report decimals 0 (a plain SELECT's say 31).
- NULL takes the other part's type; NULL with NULL is VAR_STRING binary 0.
- Flags: NOT_NULL only when every part is NOT NULL; numbers carry no
  BINARY flag; temporals carry BINARY, and NO_DEFAULT_VALUE when NOT NULL.
- Strings compare by the joined collation: 'a ' and 'a' are two rows under
  utf8mb4_0900_ai_ci (NO PAD); a DOUBLE as a string is
  0.30000000000000004.
