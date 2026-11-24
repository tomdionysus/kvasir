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
