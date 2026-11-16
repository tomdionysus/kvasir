# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The differential gate's first list: statements a session runs, one per
# line, in one session: with no table, then over databases and empty tables. tools/record_mysql.sh runs them on the
# reference MySQL 8.4 server and writes session.expected; differential_test
# replays them through a Kvasir session and compares columns, rows and
# errors.
#
# Allowed differences, left out of the list:
# - Kvasir's own version strings: @@version, @@version_comment, and the
#   server's build (@@version_compile_os, @@version_compile_machine).
# - @@performance_schema: Kvasir has none, and says 0.
# - VERSION(), USER(), CURRENT_USER() and CONNECTION_ID()'s values: the
#   server's and the account's own (Kvasir's accounts have no host part, so
#   CURRENT_USER() is user@%).
# - Arithmetic on a bare NULL literal (NULL+1): Kvasir types it DOUBLE with
#   length 23 and 31 decimals, MySQL with smaller lengths; the value is NULL.
# - Warnings are not compared yet (SHOW WARNINGS is not built).
# - Values the recorder cannot read back from mysql's table output (space at
#   either end, "|", the string 'NULL', control bytes).
# - Tables with what Kvasir does not yet hold: the temporal, BIT, ENUM, SET,
#   JSON and spatial types, character sets besides utf8mb4, utf8mb3 and
#   binary, engines besides InnoDB (MySQL makes a MyISAM or MEMORY table;
#   Kvasir refuses it as unknown under NO_ENGINE_SUBSTITUTION), CHECK,
#   FOREIGN KEY, generated columns, DEFAULT (expression), and CREATE TABLE
#   ... LIKE and ... SELECT.

SELECT 1, -1, 2147483648, 9223372036854775807, 9223372036854775808, -9223372036854775808, 18446744073709551615, 18446744073709551616, 007, TRUE, FALSE
SELECT 1.5, -1.5, .5, 5., 1.50, 0.0, 123456789012345678901234567890.5
SELECT 1e3, 1.5e-3, 0.1e1, -1e308, 1e14, 1e15, 1e-15, 1e-16, 1.7976931348623157e308
SELECT 'abc', '', _latin1'x', N'n', 0x41, X'4142', 0b1000001, NULL
SELECT 'a' 'b'
SELECT 1+1, 1-2, 2*3, 7 DIV 2, 7 % 3, -7 % 3, 9223372036854775807+0, 18446744073709551615-1
SELECT 1.5+2.25, 1.5-2.25, 1.5*2.25, 1+1.5, 1.5/2.25, 1/3, 10/4, 1/3/3/3, 7.5 % 2, 7 % 2.5
SELECT 99999999999999999999999999999999999999999999999999999999999999999 + 1
SELECT 1.000000000000000000000000000001 * 1.5
SELECT 1+1e0, 7e0 % 2, 1/3e0, 0.1+0.2e0
SELECT 5 DIV 2.5, 7.5 DIV 2, 1e0 DIV 2
SELECT -18446744073709551615, - 9223372036854775808, -(-9223372036854775808), - -1, -0, -0.0, -1e0
SELECT ~0, 1|2, 6&3, 1<<3, 5^3, ~1.5, 1.5 | 0
SELECT 1=1, 1<2, 1<=>NULL, 1=NULL, NOT 1, NOT NULL, 1 AND 0, 1 AND NULL, 0 AND NULL, 1 OR NULL, NULL OR 0, 1 XOR 1
SELECT 1 IS NULL, NULL IS NULL, 1 IS TRUE, 0 IS FALSE, NULL IS UNKNOWN, 1 IS NOT NULL, 2 IN (1,2), 3 NOT IN (1,2), 2 BETWEEN 1 AND 3, 5 NOT BETWEEN 1 AND 3
SELECT '1'+1, '1.5x'+0, 'abc'*2
SELECT 1/0, 1.5/0, 1 DIV 0, 1 % 0, 1/0e0
SELECT 9223372036854775807+1
SELECT -9223372036854775808 - 1
SELECT 18446744073709551615 + 1
SELECT 0 - 18446744073709551615
SELECT 1e308*10
SELECT 1 LIMIT 0
SELECT 1 WHERE 0
SELECT 1 FROM DUAL WHERE 1
SELECT 1, 2 LIMIT 1 OFFSET 1
SELECT 1, 2 LIMIT 1, 1
SELECT 1 AS one, 2 two, 3 'three', 1+1
SELECT *
SELECT a FROM t
SELECT * FROM nope.t
SELECT 1 ORDER BY nope
SELEC 1
SELECT select
USE nope
SELECT DATABASE()
SELECT @@session.auto_increment_increment AS auto_increment_increment, @@character_set_client AS character_set_client, @@character_set_connection AS character_set_connection, @@character_set_results AS character_set_results, @@collation_connection AS collation_connection
SELECT @@max_allowed_packet AS max_allowed_packet, @@sql_mode AS sql_mode, @@time_zone AS time_zone, @@transaction_isolation AS transaction_isolation, @@wait_timeout AS wait_timeout, @@net_write_timeout AS net_write_timeout
SELECT @@lower_case_table_names, @@autocommit, @@interactive_timeout, @@net_buffer_length, @@license, @@init_connect, @@div_precision_increment, @@transaction_read_only, @@system_time_zone
SELECT @@global.autocommit, @@local.sql_mode
SELECT @@session.version_comment
SELECT @@nope
SET @i = 1, @d = 1.5, @r = 1e0, @s = 'abc', @n = NULL, @u = 18446744073709551615
SELECT @i, @d, @r, @s, @n, @u, @i + 1, @s + 0, @d + 1, @d * 2, @nope
SET @a = 1, @b = @a + 1
SELECT @b
SET @b = @a + 1
SELECT @b, @A
SET autocommit = 0
SELECT @@autocommit
SET autocommit = ON
SELECT @@autocommit
SET autocommit = OFF
SELECT @@autocommit
SET @@session.autocommit = DEFAULT
SELECT @@autocommit
SET autocommit = 2
SET autocommit = 'x'
SET autocommit = 1.5
SET version_comment = 'x'
SET max_allowed_packet = 1024
SET nope = 1
SET sql_mode = 'ansi_quotes,real_as_float'
SELECT @@sql_mode
SET sql_mode = 'ANSI'
SELECT @@sql_mode
SET sql_mode = 'TRADITIONAL'
SELECT @@sql_mode
SET sql_mode = ''
SELECT @@sql_mode
SET sql_mode = 3
SELECT @@sql_mode
SET sql_mode = 'NOPE'
SET sql_mode = DEFAULT
SELECT @@sql_mode
SET wait_timeout = 0
SELECT @@wait_timeout
SET wait_timeout = DEFAULT
SELECT @@wait_timeout
SET transaction_isolation = 'read-committed'
SELECT @@transaction_isolation
SET transaction_isolation = 'nope'
SET NAMES nope
SET NAMES utf8mb4 COLLATE utf8mb4_bin
SELECT @@character_set_client, @@collation_connection, 'a'
SET NAMES DEFAULT
SELECT @@collation_connection
SET CHARACTER SET utf8mb4
SELECT @@character_set_client, @@character_set_connection, @@collation_connection
BEGIN
SELECT 1
COMMIT
START TRANSACTION
ROLLBACK
SELECT 1/0, '1.5x'+0
SHOW WARNINGS
SHOW WARNINGS LIMIT 1
SHOW COUNT(*) WARNINGS
SELECT @@warning_count, @@error_count
SELECT nope
SHOW ERRORS
SHOW COUNT(*) ERRORS
SELECT 1
SHOW WARNINGS
SELEC 1
SHOW WARNINGS
SELECT CONNECTION_ID() > 0, LAST_INSERT_ID(), ROW_COUNT(), FOUND_ROWS(), DATABASE(), SCHEMA()
SET @x = 1
SELECT ROW_COUNT()
SELECT 'a' = 'A', 'a' < 'b', 'B' > 'a', 'a' != 'á', 'a' <=> 'A', 'a' = 'a ', 'ß' = 'ss', 'Æ' = 'AE', 'abc' < 'abd'
SELECT 'a' IN ('A', 'b'), 'b' BETWEEN 'A' AND 'C', 'a' IN ('b', NULL), NULL = 'a', 'z' NOT IN ('a', 'b')
SELECT X'61' = 'a', X'41' = 'a', _binary'a' = 'A', _utf8mb4'a' = 'A', 'a' = 1, '1' = 1
SET @cmp = 'x'
SELECT @cmp = 'X', @cmp < 'y', @never = 'x'
SET NAMES utf8mb4 COLLATE utf8mb4_bin
SELECT 'a' = 'A', 'a' = 'a ', 'a' < 'B', @cmp = 'X'
SET NAMES utf8mb4 COLLATE utf8mb4_0900_as_cs
SELECT 'a' = 'A', 'a' < 'B', 'A' < 'a', 'á' > 'a', @cmp = 'X'
SET NAMES binary
SELECT 'a' = 'A', 'a' = 'a ', 'b' > 'a'
SET NAMES DEFAULT
SELECT @@character_set_client = 'UTF8MB4', @@character_set_client = 'utf8mb4 ', @@sql_mode = 'x', USER() = 'x', DATABASE() = 'x'
SELECT @@character_set_client = 'ütf8mb4', @@character_set_client < 'v', @cmp = @@character_set_client
SET NAMES utf8mb4 COLLATE utf8mb4_bin
SELECT 'a\t' < 'a', 'a' < 'a\t', 'a' = 'a  ', 'é' > 'f'
SET NAMES utf8mb4 COLLATE utf8mb4_general_ci
SELECT 'a' = 'A', 'ß' = 's', 'ß' = 'ss', 'a' = 'a  ', 'a\t' < 'a', 'é' = 'E', 'й' = 'и', 'ё' = 'Е'
SELECT 'a' LIKE 'A', 'ß' LIKE 's', 'a ' LIKE 'a', 'é' LIKE 'E'
SET NAMES DEFAULT
SELECT 'a' LIKE 'A', 'á' LIKE 'a', 'ß' LIKE 'ss', 'ss' LIKE 'ß', 'ß' LIKE '_', 'ss' LIKE '_', 'a ' LIKE 'a', 'a' LIKE 'a '
SELECT 'abc' LIKE 'a%', 'abc' LIKE '%c', 'abc' LIKE 'a_c', 'abc' LIKE 'a__c', '' LIKE '%', '' LIKE '_', 'a%c' LIKE 'a\%c', 'abc' LIKE 'a\%c', 'a_c' LIKE 'a|_c' ESCAPE '|', 'abc' NOT LIKE 'a%'
SELECT NULL LIKE 'a', 'a' LIKE NULL, 10 LIKE '1%', 1.5 LIKE '1.%', 'Æ' LIKE 'AE', 'Æ' LIKE 'Æ', 'x' LIKE '%%%x', 'a\\b' LIKE 'a\\\\b', 'ab' LIKE 'a\\'
SELECT 'ǆ' LIKE 'dž', 'ǆ' LIKE '_', 'ǆ' LIKE '__', 'ǅ' LIKE 'ǆ', '😀' LIKE '_', 'a' LIKE 'a%%', 'abc' LIKE 'A%C', 'é' LIKE 'e_', 'é' LIKE 'e'
SELECT 'a%c' LIKE 'a\\%c' ESCAPE '', 'a\\b' LIKE 'a\\_' ESCAPE '', @@character_set_client LIKE 'UTF%', X'61' LIKE 'a', X'61' LIKE 'A'
SELECT 'a' LIKE 'b' ESCAPE 'xy'
SET NAMES utf8mb4 COLLATE utf8mb4_bin
SELECT 'a' LIKE 'A', 'a ' LIKE 'a', 'a' LIKE 'a ', 'é' LIKE '_'
SET NAMES binary
SELECT 'a' LIKE 'A', 'é' LIKE '_', 'é' LIKE '__'
SET NAMES DEFAULT
SELECT 'a' COLLATE utf8mb4_bin, 'a' = 'A' COLLATE utf8mb4_bin, 'a' COLLATE utf8mb4_bin = 'A', 'a' COLLATE utf8mb4_0900_as_cs < 'B', 'ß' = 'ss' COLLATE utf8mb4_general_ci
SELECT 'a' COLLATE utf8mb4_general_ci, 'a' COLLATE 'utf8mb4_bin', 'a' COLLATE utf8mb4_BIN = 'A', 1 COLLATE utf8mb4_bin
SELECT X'61' = 'a' COLLATE utf8mb4_bin, X'41' = 'a' COLLATE utf8mb4_general_ci, 'a' COLLATE utf8mb4_bin LIKE 'A'
SELECT 'a' COLLATE utf8mb4_bin = 'A' COLLATE utf8mb4_general_ci
SELECT 'a' COLLATE utf8mb4_bin IN ('A', 'a' COLLATE utf8mb4_general_ci)
SELECT 'a' COLLATE latin1_swedish_ci
SELECT 'a' COLLATE nope
SELECT 'a' COLLATE binary
SELECT NULL COLLATE utf8mb4_bin
SELECT @@character_set_client COLLATE utf8mb4_bin
SELECT @@character_set_client = 'UTF8MB4' COLLATE utf8mb4_bin, @cmp COLLATE utf8mb4_bin = 'X'
SET NAMES utf8mb4 COLLATE utf8mb4_bin
SELECT _utf8mb4'a' = 'A'
SET @tie = 'x'
SET NAMES utf8mb4 COLLATE utf8mb4_0900_as_cs
SELECT _utf8mb4'a' = 'A'
SET NAMES DEFAULT
SELECT @tie = @cmp
SET @tie2 = 'x'
SET NAMES utf8mb4 COLLATE utf8mb4_0900_as_cs
SET @tie3 = 'x'
SET NAMES DEFAULT
SELECT @tie2 = @tie3
CREATE DATABASE kv_gate
CREATE DATABASE kv_gate
CREATE DATABASE IF NOT EXISTS kv_gate
CREATE SCHEMA kv_Gate
CREATE DATABASE kv_gate2 CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
CREATE DATABASE kv_gate4 COLLATE nope
CREATE DATABASE kv_gate5 CHARACTER SET utf8mb4 COLLATE latin1_swedish_ci
CREATE DATABASE `kv gate `
CREATE DATABASE information_schema
SHOW DATABASES LIKE 'kv_gate%'
SHOW SCHEMAS LIKE 'kv\_gate_'
USE kv_gate
SELECT DATABASE()
SELECT * FROM kv_gate.t
SELECT * FROM t
SELECT * FROM kv_none.t
DROP DATABASE kv_gate
SELECT DATABASE()
DROP DATABASE kv_gate
DROP DATABASE IF EXISTS kv_gate
USE kv_gate
DROP SCHEMA kv_Gate
DROP DATABASE kv_gate2
SHOW DATABASES LIKE 'kv_gate%'
CREATE DATABASE kv_tables
USE kv_tables
CREATE TABLE t2 (a TINYINT, b SMALLINT, c MEDIUMINT, d INT, e BIGINT, f TINYINT UNSIGNED, g INT UNSIGNED ZEROFILL, h BIGINT UNSIGNED, i INT(5), j BOOL, k BOOLEAN, l INTEGER)
SELECT * FROM t2
CREATE TABLE t3 (a DECIMAL, b DECIMAL(10,2), c DECIMAL(65,30), d NUMERIC(5), e FLOAT, f DOUBLE, g FLOAT(7,3), h DOUBLE(10,4), i REAL, j DECIMAL(5,2) UNSIGNED, k FLOAT(30), l DOUBLE PRECISION, m DEC(4,1), n FIXED(3), o DECIMAL(0), p FLOAT(255,30), q FLOAT(24))
SELECT * FROM t3
CREATE TABLE t4 (a CHAR, b CHAR(10), c VARCHAR(20), d BINARY(4), e VARBINARY(8), f TEXT, g TINYTEXT, h MEDIUMTEXT, i LONGTEXT, j BLOB, k TINYBLOB, l MEDIUMBLOB, m LONGBLOB, n VARCHAR(5) CHARACTER SET utf8mb3, o VARCHAR(5) COLLATE utf8mb4_bin, p CHAR(3) BINARY, q TEXT(100), r BLOB(300), s VARCHAR(10) CHARACTER SET binary, t NATIONAL CHAR(2), u NCHAR(2), v NVARCHAR(3), w CHAR(0))
SELECT * FROM t4
CREATE TABLE t5 (a INT NOT NULL, b INT NULL, c INT DEFAULT 5, d INT NOT NULL DEFAULT 7, e INT AUTO_INCREMENT PRIMARY KEY, f VARCHAR(3) DEFAULT 'x', g INT UNIQUE, h INT COMMENT 'hi', i INT DEFAULT NULL)
SELECT * FROM t5
CREATE TABLE t6 (a INT, b INT, PRIMARY KEY (a, b), UNIQUE KEY u (b), KEY k (a), INDEX (b))
SELECT * FROM t6
SELECT a, t6.b, kv_tables.t6.a AS x, a + 1, b = 2 FROM t6
SELECT x.* FROM t6 AS x WHERE a = 1 ORDER BY b LIMIT 2
SELECT zz FROM t6
SELECT a FROM t6 WHERE zz = 1
SELECT a FROM t6 ORDER BY zz
SELECT t6.a FROM t6 AS x
CREATE TABLE u2 (a INT NOT NULL UNIQUE, b INT)
SELECT * FROM u2
CREATE TABLE u3 (a INT NOT NULL, b INT NOT NULL, UNIQUE (a, b))
SELECT * FROM u3
CREATE TABLE u4 (a INT, b INT, UNIQUE (a, b), KEY (b, a))
SELECT * FROM u4
CREATE TABLE u5 (a INT UNIQUE KEY, b INT NOT NULL UNIQUE, c INT NOT NULL PRIMARY KEY)
SELECT * FROM u5
CREATE TABLE u26 (a VARCHAR(10), KEY (a(3)))
SELECT * FROM u26
CREATE TABLE u31 (a SERIAL)
SELECT * FROM u31
CREATE TABLE u32 (a INT SERIAL DEFAULT VALUE)
SELECT * FROM u32
CREATE TABLE w29 (a INT AUTO_INCREMENT, KEY (a))
SELECT * FROM w29
CREATE TABLE w31 (a INT, b INT AUTO_INCREMENT, KEY (b, a))
SELECT * FROM w31
CREATE TABLE w26 (a INT NOT NULL NULL)
SELECT * FROM w26
CREATE TABLE w18 (a INT DEFAULT 2.5, b INT DEFAULT -2.5, c INT DEFAULT 2.5e0, d INT DEFAULT 3.5e0, e INT DEFAULT '2.5', f INT DEFAULT ' 12 ', g INT DEFAULT X'01', h CHAR(2) DEFAULT X'6162', i BINARY(3) DEFAULT 'ab', j DOUBLE DEFAULT '1e3', k INT DEFAULT TRUE)
CREATE TABLE w22 (a TINYINT UNSIGNED DEFAULT 255, b TINYINT DEFAULT -128, c BIGINT UNSIGNED DEFAULT 18446744073709551615, d BIGINT DEFAULT -9223372036854775808)
CREATE TABLE u7 (a DECIMAL(5,2) DEFAULT 1.234)
CREATE TABLE w15 (a VARCHAR(2) DEFAULT 'ab ')
CREATE TABLE w33 (a INT, b INT, KEY (a), KEY (a, b), UNIQUE (a), UNIQUE (a))
CREATE TABLE w1 (a INT(5), b INT ZEROFILL, c FLOAT(7,3), d DOUBLE(10,2), e DECIMAL(5,2) UNSIGNED, f FLOAT UNSIGNED, g TINYINT(1), h BIGINT(20), i INT(11))
CREATE TABLE t1 (a INT)
CREATE TABLE t1 (a INT)
CREATE TABLE IF NOT EXISTS t1 (a INT, a INT)
CREATE TABLE T1 (a INT)
CREATE TABLE t11 (a INT, A INT)
CREATE TABLE t13 (a INT PRIMARY KEY, b INT PRIMARY KEY)
CREATE TABLE t29 (a INT, PRIMARY KEY (a), PRIMARY KEY (a))
CREATE TABLE t14 (a INT AUTO_INCREMENT)
CREATE TABLE t15 (a INT AUTO_INCREMENT KEY, b INT AUTO_INCREMENT UNIQUE)
CREATE TABLE w30 (a INT, b INT AUTO_INCREMENT, PRIMARY KEY (a, b))
CREATE TABLE u33 (a FLOAT AUTO_INCREMENT KEY)
CREATE TABLE u34 (a VARCHAR(5) AUTO_INCREMENT KEY)
CREATE TABLE t30 (a FLOAT(54))
CREATE TABLE t16 (a VARCHAR(70000))
CREATE TABLE t17 (a CHAR(256))
CREATE TABLE w42 (a VARCHAR(70000) CHARACTER SET binary)
CREATE TABLE t18 (a DECIMAL(66))
CREATE TABLE t19 (a DECIMAL(5,6))
CREATE TABLE w3 (a DECIMAL(5,31))
CREATE TABLE w5 (a FLOAT(256,2))
CREATE TABLE t31 (a INT(300))
CREATE TABLE kv_none.t (a INT)
CREATE TABLE t26 (a INT PRIMARY KEY, KEY (zz))
CREATE TABLE `t27 ` (a INT)
CREATE TABLE t28 (`a ` INT)
CREATE TABLE abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcde (a INT)
CREATE TABLE u27 (a TEXT, PRIMARY KEY (a))
CREATE TABLE u29 (a INT NULL PRIMARY KEY)
CREATE TABLE w9 (a INT, KEY k (a), KEY k (a))
CREATE TABLE w34 (a INT UNIQUE, b INT, KEY a (b))
CREATE TABLE w32 (a INT, `PRIMARY` INT, KEY `PRIMARY` (a))
CREATE TABLE w10 (a INT, KEY (a(2)))
CREATE TABLE w11 (a VARCHAR(5), KEY (a(6)))
CREATE TABLE w12 (a VARCHAR(1000), KEY (a))
CREATE TABLE w44 (a VARBINARY(65533))
CREATE TABLE w45 (a VARCHAR(16383), b INT)
CREATE TABLE y2 (a VARBINARY(65471), b TEXT, c LONGBLOB, d DECIMAL(20,5), e CHAR(10) CHARACTER SET binary, f TINYBLOB, g MEDIUMTEXT)
CREATE TABLE y1 (a VARBINARY(65470), b TEXT, c LONGBLOB, d DECIMAL(20,5), e CHAR(10) CHARACTER SET binary, f TINYBLOB, g MEDIUMTEXT)
CREATE TABLE u11 (a VARCHAR(5) CHARACTER SET utf8mb3 COLLATE utf8mb4_bin)
CREATE TABLE u12 (a VARCHAR(5) CHARACTER SET nope)
CREATE TABLE u13 (a VARCHAR(5) COLLATE nope)
CREATE TABLE t9 (a INT) ENGINE=Nope
CREATE TABLE t7 (a INT) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
CREATE TABLE t20 (a INT DEFAULT 'x')
CREATE TABLE t21 (a INT NOT NULL DEFAULT NULL)
CREATE TABLE t22 (a TEXT DEFAULT 'x')
CREATE TABLE t24 (a TINYINT DEFAULT 300)
CREATE TABLE t25 (a VARCHAR(2) DEFAULT 'abc')
CREATE TABLE u9 (a INT UNSIGNED DEFAULT -1)
CREATE TABLE w16 (a INT DEFAULT '12abc')
CREATE TABLE w19 (a DECIMAL(3,1) DEFAULT 99.95)
CREATE TABLE w21 (a FLOAT DEFAULT 1e40)
CREATE TABLE u35 (a INT AUTO_INCREMENT DEFAULT 3 KEY)
CREATE TABLE u6 (a INT DEFAULT 1, g INT DEFAULT CURRENT_TIMESTAMP)
CREATE TABLE information_schema.x (a INT)
CREATE TABLE t (a VARCHAR)
SHOW TABLES
SHOW TABLES LIKE 't1%'
SHOW FULL TABLES LIKE 'T%'
DROP TABLE t1
DROP TABLE t1
DROP TABLE IF EXISTS t1
DROP TABLE t2, nothere, t3
DROP TABLE t2, t2
DROP TABLE n1, t2, n2
DROP TABLE kv_none.t
DROP TABLES t2, t3
SHOW TABLES FROM kv_tables LIKE 't%'
SHOW TABLES FROM kv_none
DROP DATABASE kv_tables
CREATE TABLE t (a INT)
SHOW TABLES
DROP TABLE t
CREATE DATABASE kv_rows
USE kv_rows
CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, a INT, b VARCHAR(3) DEFAULT 'x', c DECIMAL(5,2), d INT NOT NULL)
INSERT INTO t (a, d) VALUES (1, 10)
SELECT LAST_INSERT_ID(), ROW_COUNT()
INSERT INTO t (a, d) VALUES (2, 20), (3, 30)
SELECT LAST_INSERT_ID(), ROW_COUNT()
INSERT INTO t VALUES (10, 4, 'abc', 1.5, 40)
INSERT INTO t VALUES (NULL, 5, 'y', 2, 50)
INSERT t SET a = 6, d = 60
INSERT INTO t (d) VALUES (DEFAULT)
INSERT INTO t (a) VALUES (7)
INSERT INTO t () VALUES ()
INSERT INTO t (a, d) VALUES (8)
INSERT INTO t VALUES (1)
INSERT INTO t (zz) VALUES (1)
INSERT INTO t (a, a, d) VALUES (1, 1, 1)
INSERT INTO t (id, d) VALUES (1, 1)
INSERT INTO t (b, d) VALUES ('abcd', 1)
INSERT INTO t (a, d) VALUES (99999999999, 1)
INSERT INTO t (a, d) VALUES ('x', 1)
INSERT INTO t (a, d) VALUES ('12x', 1)
INSERT INTO t (c, d) VALUES (1.234, 1)
INSERT INTO t (b, d) VALUES ('abc  ', 1)
INSERT INTO t (d) VALUES (NULL)
INSERT INTO t (a, d) VALUES (1, 2), (2, NULL)
INSERT INTO t (a, d) VALUES (100, 1), (101, 'q')
INSERT INTO t (id, d) VALUES (0, 1)
INSERT INTO kv_none.t VALUES (1)
INSERT INTO nothere VALUES (1)
SELECT * FROM t
SELECT * FROM t WHERE a > 3 ORDER BY id DESC LIMIT 2
SELECT id, a + 1, b, c * 2 FROM t WHERE b = 'X' AND id < 14
SELECT b AS x, id FROM t ORDER BY x DESC, 2 LIMIT 3
SELECT id FROM t WHERE a IS NULL ORDER BY c DESC, id
CREATE TABLE u (a INT, b CHAR(3), c VARBINARY(4), d FLOAT, e DOUBLE, f BIGINT UNSIGNED, g TEXT)
INSERT INTO u VALUES (1, 'ab ', 'x', 1.1, 1.1, 18446744073709551615, 'long text')
INSERT INTO u (a, d) VALUES (3.5e0, 3.4e38), (-2.5, 1e-10), (2.5, NULL)
SELECT * FROM u
SELECT d * 1, e + 1, b = 'ab', b LIKE 'a%' FROM u WHERE a = 1
CREATE TABLE z (a INT(5) ZEROFILL, b DECIMAL(6,2) ZEROFILL, c DOUBLE, e INT UNIQUE)
INSERT INTO z VALUES (42, 3.5, -0.0, 1)
SELECT *, a + 0 FROM z
INSERT INTO z (e) VALUES (1)
INSERT INTO z (e) VALUES (NULL), (NULL)
SELECT e FROM z ORDER BY e
INSERT INTO z (b) VALUES ('x')
INSERT INTO z (b) VALUES ('1.5x')
INSERT INTO z (c) VALUES ('x')
INSERT INTO z (b) VALUES (99999)
CREATE TABLE m (b VARCHAR(5) CHARACTER SET utf8mb3)
INSERT INTO m VALUES (X'F09F9880')
CREATE TABLE n (a INT, b VARCHAR(5))
INSERT INTO n VALUES (3, 'c'), (1, 'a'), (NULL, 'n'), (2, 'b')
SELECT * FROM n
SELECT * FROM n ORDER BY a
SELECT * FROM n ORDER BY a DESC
SELECT b FROM n ORDER BY b DESC LIMIT 1, 2
SELECT a, b FROM n WHERE b LIKE 'a%' OR a IS NULL
SELECT * FROM n AS x WHERE x.a = 1
INSERT INTO n VALUES (1, 'a'), (2)
INSERT INTO n SET a = 10, b = DEFAULT
SELECT * FROM n WHERE a >= 9
CREATE TABLE k (s VARCHAR(5) PRIMARY KEY, v INT)
INSERT INTO k VALUES ('b', 1), ('a', 2), ('C', 3)
SELECT * FROM k
INSERT INTO k VALUES ('A', 4)
INSERT INTO k VALUES ('a ', 4)
SELECT s, v FROM k WHERE s = 'a'
CREATE TABLE p (s VARCHAR(5) COLLATE utf8mb4_bin PRIMARY KEY)
INSERT INTO p VALUES ('a'), ('B'), ('a ')
SELECT * FROM p
BEGIN
INSERT INTO n VALUES (50, 'tx')
SELECT * FROM n WHERE a = 50
ROLLBACK
SELECT * FROM n WHERE a = 50
CREATE TABLE c (id INT AUTO_INCREMENT PRIMARY KEY, a INT, b VARCHAR(3) DEFAULT 'x', d INT NOT NULL, u INT UNIQUE)
INSERT INTO c (a, d, u) VALUES (1, 10, 1), (2, 20, 2), (3, 30, 3), (4, 40, NULL)
UPDATE c SET a = a + 10 WHERE id <= 2
SELECT ROW_COUNT()
UPDATE c SET a = a WHERE id = 1
SELECT ROW_COUNT()
UPDATE c SET b = 'abcd' WHERE id = 1
UPDATE c SET d = NULL WHERE id = 1
UPDATE c SET u = 2 WHERE id = 1
UPDATE c SET id = 2 WHERE id = 1
UPDATE c SET zz = 1
UPDATE c SET a = 'x' WHERE id = 1
UPDATE c SET b = DEFAULT WHERE id = 3
UPDATE c SET a = 5, b = 'yy' WHERE a > 100
UPDATE c SET a = a + 1 ORDER BY id DESC LIMIT 2
UPDATE c SET id = id + 10 WHERE id = 4
UPDATE c SET a = NULL, b = NULL WHERE id = 3
UPDATE kv_none.c SET a = 1
UPDATE nothere SET a = 1
UPDATE c AS x SET x.a = 7 WHERE x.id = 2
UPDATE c SET a = 1 WHERE zz = 1
UPDATE c SET d = DEFAULT WHERE id = 2
SELECT * FROM c
DELETE FROM c WHERE id = 2
SELECT ROW_COUNT()
DELETE FROM c WHERE a > 1000
DELETE FROM c ORDER BY id DESC LIMIT 1
DELETE FROM nothere
DELETE FROM c WHERE zz = 1
SELECT * FROM c
INSERT INTO c (id, a, d, u) VALUES (2, 9, 9, 2)
SELECT * FROM c
DELETE FROM c
SELECT ROW_COUNT()
INSERT INTO c (a, d) VALUES (1, 1)
SELECT LAST_INSERT_ID()
CREATE TABLE q (a INT, b INT)
INSERT INTO q VALUES (1, 1), (2, 2), (1, 3)
UPDATE q SET b = b * 10 WHERE a = 1
DELETE FROM q WHERE b = 2
SELECT * FROM q
UPDATE q SET a = 1, a = 2
SELECT * FROM q
UPDATE q SET b = a, a = b
SELECT * FROM q
BEGIN
UPDATE q SET b = 99
DELETE FROM q WHERE a = 2 LIMIT 1
SELECT * FROM q
ROLLBACK
SELECT * FROM q
DROP DATABASE kv_rows
