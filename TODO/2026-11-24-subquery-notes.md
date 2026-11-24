# Subqueries: what MySQL 8.4.13 was observed to do (2026-11-24)

Probed on the reference server over `t (id INT PRIMARY KEY, a INT, b
VARCHAR(5))` holding (1, 10, 'x'), (2, 20, 'y'), (3, NULL, 'x').

## Scalar subqueries

- `(SELECT 1)` is LONGLONG length 2, NOT_NULL BINARY NUM; `(SELECT 'x')`
  VAR_STRING length 4, decimals 31, NOT_NULL; `(SELECT NULL)` NULL type;
  `(SELECT 1 FROM DUAL WHERE FALSE)` LONGLONG length 2, nullable, NULL;
  `(SELECT a FROM t WHERE id = 1)` LONG 11, nullable; `(SELECT MAX(a) FROM
  t)` LONG 11.
- More than one row is 1242 (21000) "Subquery returns more than 1 row";
  more than one column 1241 (21000) "Operand should contain 1 column(s)".
- Correlated: `SELECT id, (SELECT b FROM t u WHERE u.id = t.id + 1) nxt
  FROM t t` reads the outer row (y, x, NULL); nxt is VAR_STRING 20,
  decimals 31, nullable.

## IN, EXISTS, ANY and ALL

- `a IN (SELECT a FROM t WHERE b = 'x')` keeps id 1 (NULL never matches);
  `a NOT IN (...)` over a set holding NULL keeps nothing.
- `1 IN (SELECT a FROM t)` is NULL (no match, a NULL in the set); `10 IN`
  1; `NULL IN` NULL; `1 NOT IN` NULL. LONGLONG length 1, BINARY NUM,
  nullable.
- `EXISTS (SELECT 1)` 1, over no row 0, `NOT EXISTS (SELECT * FROM t)` 0:
  LONGLONG length 1, NOT_NULL BINARY NUM.
- `15 > ANY (SELECT a FROM t)` 1; `15 > ALL` 0; `5 < ALL (... a IS NOT
  NULL)` 1; `15 = SOME` NULL (no match, a NULL); `1 > ALL (no rows)` 1.
  LONGLONG length 1, nullable.
- Rows: `(1, 2) = (SELECT 1, 2)` 1 (NOT_NULL); `(1, 2) IN (SELECT 1, 2)`
  1; `WHERE (a, b) = (SELECT 10, 'x')` keeps a 10.

## Derived tables

- `FROM (SELECT 1 x, 'a' y UNION SELECT 2, 'b') q` reads the union's
  rows; its columns as the union typed them (LONGLONG 2, VAR_STRING 4).
- `SELECT * FROM (SELECT 1) q`: one column named `1`, LONG length 2,
  NOT_NULL NUM (a derived table's field: an integer literal is LONG).
- No alias is 1248 (42000) "Every derived table must have its own alias".
- `SELECT q.a FROM (SELECT a, b FROM t WHERE a > 5) AS q`: a LONG 11,
  nullable, NUM (no BINARY).

## A derived table's columns (build/derived-matrix.txt, 43 items)

`SELECT x FROM (SELECT <item> AS x FROM t) q`: MySQL merges the derived
table into the outer query, and each column reports:
- a table column as its field does: its own flags (i NOT NULL keeps
  NO_DEFAULT_VALUE), no BINARY on numbers (TINY 4 NUM, LONG 11);
- an expression as its item does, with its BINARY flag, an integer
  literal one longer than a plain SELECT's (1 is 2, 1234567890 is 11, 1+1
  3, i+1 12), TRUE 1, 1e0 DOUBLE 3;
- strings with decimals 31 for expressions ('xy', CONCAT), 0 for columns
  and MAX(vc); X'41' VAR_STRING binary length 1 flagged UNSIGNED BINARY;
- dates and times with the result collation and their length as
  characters times 4 (DATE 40, TIME(2) 52, DATETIME(3) 92, TIMESTAMP 76,
  NOW() 76), still BINARY, decimals their fsp;
- aggregates: COUNT(*) LONGLONG 21 NOT_NULL NUM (no BINARY), SUM(i)
  NEWDECIMAL 33, AVG(i) NEWDECIMAL 16,4, MAX(vc) VAR_STRING 40.
A derived table with no FROM is materialised instead: `SELECT * FROM
(SELECT 1) q` is LONG length 2 NOT_NULL NUM.
