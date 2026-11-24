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
CREATE DATABASE kv_groups
USE kv_groups
CREATE TABLE t (id INT PRIMARY KEY, g VARCHAR(10), i INT, u INT UNSIGNED, b BIGINT, d DECIMAL(6,2), f DOUBLE, s VARCHAR(20), ti TINYINT)
INSERT INTO t VALUES (1,'a',10,1,100,1.50,1.5,'x',1),(2,'a',20,2,200,2.25,2.5,'y',2),(3,'b',NULL,3,NULL,NULL,NULL,NULL,NULL),(4,'b',5,4,5,0.10,0.1,'z',3),(5,NULL,7,5,7,7.00,7,'w',4)
CREATE TABLE v (id INT PRIMARY KEY, k INT NOT NULL, n INT, d DECIMAL(6,2), f DOUBLE, c CHAR(3), s VARCHAR(5) NOT NULL DEFAULT '', z INT(4) ZEROFILL, bs VARBINARY(4), tx TEXT)
INSERT INTO v VALUES (1,1,1,1.00,1,'a','x',1,'p','t'),(2,1,2,2.00,2,'a','y',2,'q','u'),(3,2,3,3.00,3,'b','z',3,'r','v')
CREATE TABLE w (id INT PRIMARY KEY, a CHAR(2) NOT NULL, b VARCHAR(3), c VARCHAR(3) NOT NULL, e TEXT, i INT, d DECIMAL(4,1) NOT NULL, x DOUBLE)
INSERT INTO w VALUES (1,'p','q','r','s',1,1.0,1),(2,'p','q','r','s',2,2.0,2)
SELECT COUNT(*), COUNT(i), COUNT(DISTINCT g), SUM(i), SUM(u), SUM(b), SUM(d), SUM(f), SUM(ti) FROM t
SELECT AVG(i), AVG(u), AVG(b), AVG(d), AVG(f), AVG(ti) FROM t
SELECT MIN(i), MAX(i), MIN(u), MAX(d), MIN(f), MAX(s), MIN(g), MAX(ti), MIN(b) FROM t
SELECT MIN(z), MAX(c), MIN(bs), MIN(tx), MAX(f), MIN(n) FROM v
SELECT COUNT(*), SUM(i), AVG(i), MIN(i), MAX(s) FROM t WHERE id > 100
SELECT 1 + COUNT(*), SUM(i) / COUNT(i), SUM(i) * 2 FROM t
SELECT SUM(DISTINCT i), AVG(DISTINCT u), COUNT(DISTINCT g, s) FROM t
SELECT COUNT(DISTINCT k), COUNT(DISTINCT c) FROM v
SELECT SUM(k > 1), COUNT(n = 2), SUM(-n), AVG(n * 1.5) FROM v
SELECT SUM(9223372036854775807), SUM(18446744073709551615) FROM v
SELECT SUM(c), SUM(s) FROM v
SHOW WARNINGS
SELECT SUM('1x') FROM v
SHOW WARNINGS
SELECT BIT_AND(i), BIT_OR(i), BIT_XOR(i), STD(i), VARIANCE(i) FROM t
SELECT STDDEV_SAMP(i), VAR_SAMP(i), STDDEV_POP(i), VAR_POP(i), STDDEV(i) FROM t
SELECT BIT_AND(i), BIT_OR(i), STD(i), VAR_SAMP(i) FROM t WHERE id > 100
SELECT COUNT(*)
SELECT SUM(1), MIN(2), AVG(3)
SELECT COUNT(*) FROM v HAVING COUNT(*) > 5
SELECT COUNT(*), k FROM v WHERE k = 1
SELECT g, COUNT(*), SUM(i) FROM t GROUP BY g
SELECT g, COUNT(*) AS n FROM t GROUP BY g ORDER BY n DESC, g
SELECT g, SUM(i) FROM t GROUP BY g HAVING SUM(i) > 10
SELECT g, SUM(i) s FROM t GROUP BY g HAVING s > 10
SELECT COUNT(*) FROM t GROUP BY g
SELECT g FROM t GROUP BY 1
SELECT g AS k, COUNT(*) FROM t GROUP BY k
SELECT SUM(id) FROM t GROUP BY g ORDER BY g
SELECT k FROM v GROUP BY k HAVING COUNT(*) > 1
SELECT k AS x FROM v GROUP BY k HAVING x > 1
SELECT k FROM v GROUP BY k HAVING k > 1
SELECT COUNT(*) AS cnt FROM v GROUP BY k HAVING cnt > 1
SELECT k FROM v GROUP BY k ORDER BY SUM(n) DESC
SELECT COUNT(*) FROM v GROUP BY k HAVING SUM(n) > 0 ORDER BY k DESC LIMIT 1
SELECT k, COUNT(*) FROM v GROUP BY k ORDER BY COUNT(*) DESC, k LIMIT 1 OFFSET 1
SELECT k + 1 FROM v GROUP BY k + 1
SELECT k * 2 FROM v GROUP BY k
SELECT 1 FROM v GROUP BY k
SELECT k FROM v GROUP BY K
SELECT v.k FROM v GROUP BY k
SELECT MAX(n), MIN(n) FROM v GROUP BY k ORDER BY MAX(n)
SELECT MIN(c), MAX(c) FROM v WHERE id > 99 GROUP BY k
SELECT k, c FROM v GROUP BY id
SELECT n, COUNT(*) FROM v WHERE n = 2 GROUP BY k
SELECT DISTINCT k FROM v
SELECT DISTINCT c, k FROM v
SELECT DISTINCT c FROM v ORDER BY c DESC
SELECT DISTINCT k + 1 FROM v
SELECT DISTINCT * FROM w
SELECT k, n, d, f, c, s, z, bs, id FROM v GROUP BY id
SELECT k, k + 1, 'lit', 5, 2.5, NULL, COUNT(*), SUM(n), MIN(c), MAX(s), MIN(z), MAX(bs), AVG(d) FROM v GROUP BY k
SELECT k AS kk, MIN(tx), COUNT(n) FROM v GROUP BY kk
SELECT a, b, c FROM w GROUP BY a, b, c
SELECT e FROM w GROUP BY e
SELECT b AS bb, a FROM w GROUP BY bb, a
SELECT MIN(id), MAX(a), MIN(c), MIN(e), MIN(i + 1), MAX(d), MIN(x) FROM w GROUP BY b
SELECT COUNT(*) + 0.5, -COUNT(*), COUNT(*) * k FROM v GROUP BY k
SELECT 1 + 1, a, 'x' FROM w GROUP BY a
SELECT MIN(1), MAX('s'), SUM(2), COUNT(3) FROM w GROUP BY a
SELECT a, COUNT(*) FROM w WHERE a = 'p' GROUP BY a
SELECT b, COUNT(*) FROM w WHERE b = 'q' GROUP BY b
SELECT a, COUNT(*) FROM w WHERE a = 'p' AND id > 0 GROUP BY a, b
SELECT a, b, i + 1, COUNT(DISTINCT i), SUM(i), MIN(i), MAX(b) FROM w GROUP BY a, b, i + 1
SELECT g, i FROM t GROUP BY g
SELECT k FROM v GROUP BY k ORDER BY n
SELECT id, COUNT(*) FROM t
SELECT COUNT(*) FROM t WHERE COUNT(*) > 1
SELECT COUNT(COUNT(*)) FROM v
SELECT MAX(n) FROM v GROUP BY MAX(n)
SELECT k FROM v GROUP BY k HAVING n > 1
SELECT SUM(n) FROM v GROUP BY 2
SELECT k FROM v GROUP BY nope
SET SESSION sql_mode = ''
SELECT k, n FROM v GROUP BY k ORDER BY k
SELECT k, COUNT(*) + 1 AS y, 'z' AS w FROM v GROUP BY k ORDER BY y, k
SELECT id, COUNT(*) FROM v
SET SESSION sql_mode = DEFAULT
DROP DATABASE kv_groups
SELECT IFNULL(1,2), IFNULL(NULL,2), IFNULL(1,2.5), IFNULL(1,'ab'), IFNULL('abc',1), IFNULL(NULL,NULL), IFNULL(1.5e0,2)
SELECT IFNULL(-12,3), IFNULL(18446744073709551615,1), IFNULL(18446744073709551615,0x01), IFNULL(2.5,'a'), IFNULL(1.5e0,'a'), IFNULL(2.5,1.5e0)
SELECT IFNULL(12.345,123.4), IFNULL(0x41,'a'), IFNULL(0x41,1), IFNULL(_binary'ab','a'), IFNULL(TRUE,5), IFNULL(1.5,2.25)
SELECT COALESCE(NULL,1), COALESCE(NULL,NULL,'x'), COALESCE(1,2.25), COALESCE(NULL), COALESCE(1,NULL)
SELECT NULLIF(1,1), NULLIF(1,2), NULLIF('a','A'), NULLIF(2.5,1), NULLIF(NULL,1), NULLIF(1,2.5)
SELECT IF(1,2,3), IF(0,'a','bc'), IF(NULL,1,2.5), IF(1,NULL,2), IF('x',1,2), IF(1,1,2.5), IF(1,1.5,2.25)
SHOW WARNINGS
SELECT CASE 1 WHEN 1 THEN 'a' ELSE 'bcd' END, CASE WHEN 0 THEN 1 END, CASE WHEN 1 THEN 1 ELSE 2.5 END, CASE 'a' WHEN 'A' THEN 1 ELSE 0 END
SELECT CASE 1 WHEN 1.0 THEN 'x' WHEN '1' THEN 'y' END, CASE '1a' WHEN 1 THEN 'num' ELSE 'no' END, CASE 1 WHEN NULL THEN 'x' ELSE 'y' END
SHOW WARNINGS
SELECT IFNULL(1, 1/0), COALESCE(NULL, 1, 1/0), IF(1, 2, 1/0), CASE WHEN 1 THEN 2 ELSE 1/0 END
SELECT IFNULL('a' COLLATE utf8mb4_bin, 'b' COLLATE utf8mb4_0900_as_cs)
SELECT COALESCE('a' COLLATE utf8mb4_bin, 'b' COLLATE utf8mb4_0900_as_cs, 'c')
SELECT CASE 'a' COLLATE utf8mb4_bin WHEN 'b' COLLATE utf8mb4_0900_as_cs THEN 1 END
SELECT NULLIF(1)
SELECT IF(1,2)
SELECT COALESCE()
SELECT _binary'ab', _binary'12' + 1
CREATE DATABASE kv_flow
USE kv_flow
CREATE TABLE f (i INT, u INT UNSIGNED NOT NULL, b BIGINT, d DECIMAL(6,2), v VARCHAR(10), c CHAR(3) COLLATE utf8mb4_bin, t TINYINT, x TEXT, db DOUBLE, vb VARBINARY(5))
INSERT INTO f VALUES (1, 2, 3, 4.5, 'abc', 'x', 7, 'tt', 1.25, 'zz'), (NULL, 3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)
SELECT IFNULL(i,u), IFNULL(u,u), IFNULL(t,t), IFNULL(t,i), IFNULL(i,b), IFNULL(d,i), IFNULL(v,c), IFNULL(v,x), IFNULL(v,i), COALESCE(i,v,d), IFNULL(db,d), IFNULL(vb,v), IFNULL(u,-1), IFNULL(c,c) FROM f
SELECT IFNULL(db,'a'), IFNULL(c,'a'), IFNULL(x,x), IFNULL(i,'a'), IFNULL(d,'a'), IFNULL(c,v), IFNULL(v,_binary'a'), IFNULL(t,1), IFNULL(i,1.5), IFNULL(u,1.5), IFNULL(b,1.5), IFNULL(t,2.5), IFNULL(i,1e0), IFNULL(c,1) FROM f
SELECT NULLIF(i,1), NULLIF(v,'x'), NULLIF(u,2), NULLIF(t,1), NULLIF(c,1), NULLIF(x,1), NULLIF(db,1) FROM f
SELECT IF(i,v,c), IF(1,t,t), CASE i WHEN 1 THEN u ELSE t END, CASE WHEN i THEN x END, IF(1, i, d), COALESCE(i, d), IFNULL(d, 1.123) FROM f
SELECT u, CASE WHEN i IS NULL THEN 'none' WHEN i > 0 THEN 'some' END AS k FROM f WHERE IFNULL(i, 0) = 0 OR COALESCE(v, '') = 'abc' ORDER BY IFNULL(i, 99)
DROP DATABASE kv_flow
SELECT CONCAT('a','bc'), CONCAT('a',1), CONCAT('a',NULL), CONCAT(1,2.5), CONCAT(0x41,'b'), CONCAT('a' COLLATE utf8mb4_bin,'b'), CONCAT(1.5e0), CONCAT(NULL), CONCAT(-12)
SELECT CONCAT_WS(',','a','b'), CONCAT_WS(',','a',NULL,'b'), CONCAT_WS(NULL,'a'), CONCAT_WS(',',1,2), CONCAT_WS('--','a')
SELECT LENGTH('abc'), LENGTH('é'), LENGTH(NULL), LENGTH(123), OCTET_LENGTH('é'), CHAR_LENGTH('é'), CHARACTER_LENGTH('abc'), CHAR_LENGTH(0x4142), LENGTH(1.5e0)
SELECT CONCAT('a' COLLATE utf8mb4_bin, 'b' COLLATE utf8mb4_0900_as_cs)
SELECT CONCAT()
SELECT CONCAT_WS(',')
SELECT LENGTH(1,2)
CREATE DATABASE kv_strings
USE kv_strings
CREATE TABLE f (i INT, u INT UNSIGNED NOT NULL, d DECIMAL(6,2), v VARCHAR(10), c CHAR(3) COLLATE utf8mb4_bin, x TEXT, db DOUBLE, vb VARBINARY(5))
INSERT INTO f VALUES (1, 2, 4.5, 'abc', 'x', 'tt', 1.25, 'zz'), (NULL, 3, NULL, NULL, NULL, NULL, NULL, NULL)
SELECT CONCAT(v,c), CONCAT(i,v), CONCAT(u), CONCAT(x,'a'), CONCAT(vb,v), CONCAT(c), CONCAT(d), CONCAT(db), CONCAT_WS(v,c,i), LENGTH(v), CHAR_LENGTH(x), LENGTH(u), CONCAT(v, v, v, v) FROM f
CREATE TABLE g (a VARCHAR(100), m VARCHAR(8000), t TINYTEXT, bl BLOB, vb VARBINARY(300), mt MEDIUMTEXT, x TEXT)
SELECT CONCAT(a,a), CONCAT(m,m), CONCAT(m,m,m,m,m), CONCAT(t), CONCAT(bl), CONCAT(x), CONCAT(vb, vb), CONCAT(mt), CONCAT(x,x,x,x,x) FROM g
SELECT SUBSTRING(v,2), SUBSTRING(v,i), SUBSTRING(v,1,i), LEFT(v,2), RIGHT(c,1), LEFT(x,3), SUBSTRING(i,1,1), LEFT(vb,1), SUBSTRING(v,-1), LEFT(d, 2), MID(v FROM 2 FOR 1) FROM f
DROP DATABASE kv_strings
SELECT SUBSTRING('abcdef',2), SUBSTRING('abcdef',2,3), SUBSTRING('abcdef',-2), SUBSTRING('abcdef',0), SUBSTRING('abcdef',2,-1), SUBSTRING('abcdef',-10), SUBSTRING('abcdef',7), SUBSTRING('abcdef',3,100)
SELECT SUBSTRING('abcdef',2.5), SUBSTRING('abcdef',1.5e0), SUBSTR('héllo',2,2), MID('abc',2,1), SUBSTRING('abcdef' FROM 3), SUBSTRING('abcdef' FROM 2 FOR 2), SUBSTRING(12345,2,2), SUBSTRING(NULL,1)
SELECT SUBSTRING('abc',NULL), SUBSTRING('abc',1,NULL), SUBSTRING(_binary'héllo',2,2), LEFT('abcdef',2), RIGHT('abcdef',2), LEFT('abc',-1), RIGHT('abc',-1), LEFT('abc',10), LEFT(NULL,1)
SELECT LEFT('abc',NULL), LEFT(12345,2), RIGHT('héllo',3), LEFT('abc',1.5), RIGHT(_binary'héllo',2), LEFT(_binary'ab',1)
SELECT SUBSTRING('a')
SELECT LEFT('a')
SELECT TRIM('  ab  '), LTRIM('  ab  '), RTRIM('  ab  '), TRIM(LEADING 'x' FROM 'xxabxx'), TRIM(TRAILING 'x' FROM 'xxabxx'), TRIM(BOTH 'xy' FROM 'xyxyabxy'), TRIM('x' FROM 'xabx'), TRIM(NULL), TRIM(12), TRIM(_binary' a ')
SELECT TRIM('a' FROM NULL), TRIM(NULL FROM 'a'), TRIM(LEADING FROM '  a'), TRIM(TRAILING '' FROM 'a'), TRIM(BOTH 'ab' FROM 'ababa')
SELECT REPLACE('abcabc','b','XY'), REPLACE('abc','','x'), REPLACE('aAa','a','b'), REPLACE(NULL,'a','b'), REPLACE('abc','b',NULL), REPLACE(123,2,9), REPLACE('ab','b','xyz')
SELECT LOCATE('b','abcb'), LOCATE('b','abcb',3), LOCATE('B','abc'), LOCATE('','abc'), LOCATE('','abc',4), LOCATE('','abc',5), LOCATE('x','abc'), LOCATE(NULL,'a'), LOCATE('b','abc',0), LOCATE('b','abc',NULL)
SELECT INSTR('abcb','c'), POSITION('c' IN 'abc'), LOCATE('é','aéb')
SELECT STRCMP('text', 'text2'), STRCMP('text2', 'text'), STRCMP('text', 'text'), STRCMP(NULL, 'a'), STRCMP('a', NULL), STRCMP(1, 2), STRCMP(10, 9), STRCMP('B', 'a'), STRCMP(_binary'B', 'a')
SET @s1 = _utf8mb4 'x' COLLATE utf8mb4_0900_ai_ci, @s2 = _utf8mb4 'X' COLLATE utf8mb4_0900_ai_ci, @s3 = _utf8mb4 'x' COLLATE utf8mb4_0900_as_cs, @s4 = _utf8mb4 'X' COLLATE utf8mb4_0900_as_cs
SELECT STRCMP(@s1, @s2), STRCMP(@s3, @s4), STRCMP(@s1, @s3 COLLATE utf8mb4_0900_ai_ci)
SELECT STRCMP(@s1, @s3)
SELECT STRCMP('a')
SELECT LENGTH('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'), 'éééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééé'
SELECT 1 AS '  x', '  y', 2 AS ' '
SHOW WARNINGS
SELECT 1 AS bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
SHOW WARNINGS
CREATE DATABASE kv_strcmp
CREATE TABLE kv_strcmp.t (a INT)
SELECT TABLE_NAME, IF(STRCMP(TABLE_TYPE,'BASE TABLE'), IF(STRCMP(TABLE_TYPE,'SYSTEM VIEW'), TABLE_TYPE, 'VIEW'), 'TABLE') AS TABLE_TYPE FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = 'kv_strcmp'
DROP DATABASE kv_strcmp
CREATE DATABASE kv_distinct
CREATE TABLE kv_distinct.t (a INT PRIMARY KEY, b INT, c INT)
INSERT INTO kv_distinct.t VALUES (1, 2, 3), (2, 2, 1), (3, 4, 2)
SELECT DISTINCT b FROM kv_distinct.t ORDER BY c
SELECT DISTINCT b FROM kv_distinct.t ORDER BY b, c
SELECT DISTINCT b FROM kv_distinct.t AS u ORDER BY ABS(c) DESC
SELECT DISTINCT b + c FROM kv_distinct.t ORDER BY c
SELECT DISTINCT b FROM kv_distinct.t ORDER BY b + 1 DESC
SELECT DISTINCT b AS x FROM kv_distinct.t ORDER BY x DESC
SELECT DISTINCT b + c FROM kv_distinct.t ORDER BY b + c
SELECT DISTINCT b, c FROM kv_distinct.t ORDER BY c
SELECT DISTINCT * FROM kv_distinct.t ORDER BY c
DROP DATABASE kv_distinct
CREATE DATABASE kv_ignore
CREATE TABLE kv_ignore.t (id INT AUTO_INCREMENT PRIMARY KEY, u VARCHAR(3) UNIQUE, n INT NOT NULL, d TINYINT, e TINYINT UNSIGNED, m DECIMAL(4,1), f FLOAT, g DOUBLE NOT NULL, c CHAR(2), b BINARY(2), vb VARBINARY(2), tx TINYTEXT NOT NULL)
INSERT INTO kv_ignore.t (id, u, n, g, tx) VALUES (1, 'a', 1, 1, 'x')
INSERT IGNORE INTO kv_ignore.t (id, u, n, g, tx) VALUES (1, 'b', 2, 2, 'x'), (2, 'a', 3, 3, 'x'), (3, 'c', 4, 4, 'x')
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore.t (id, u, n, g, tx) VALUES (1, 'z', 1, 1, 'x')
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore.t (u, n, d, e, g, tx) VALUES ('dddd', NULL, 300, -1, NULL, NULL)
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore.t (u, d, e, g, tx) VALUES ('e', -300, 256, '1e400', REPEAT('y', 300))
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore.t (u, n, m, f, g, tx) VALUES ('f', 'x', 12345, 'abc', '7z', 'x')
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore.t (u, n, d, m, g, tx) VALUES ('h', '12x', ' 5 ', -99999.99, 1e308 * 10, 'x')
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore.t (u, n, m, g, c, b, vb, tx) VALUES ('i', 1, '1.25', 2, 'abc', 'xyz', 0x616263, 'x')
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore.t (u, n, m, g, tx) VALUES ('j', 1, '12.3x', 1, 'x'), ('c', 1, 1, 1, 'x'), ('k', 1, 1, 1, 'x')
SHOW WARNINGS
SELECT * FROM kv_ignore.t ORDER BY id
SELECT LAST_INSERT_ID()
INSERT IGNORE INTO kv_ignore.t (nope) VALUES (1)
INSERT IGNORE INTO kv_ignore.t VALUES (1)
DROP DATABASE kv_ignore
CREATE DATABASE kv_ignore2
CREATE TABLE kv_ignore2.t (k INT AUTO_INCREMENT PRIMARY KEY, i INT, m DECIMAL(4,1), mu DECIMAL(4,1) UNSIGNED, v VARCHAR(4), b BINARY(2) NOT NULL, dd DOUBLE)
INSERT IGNORE INTO kv_ignore2.t (i, m) VALUES ('12.7x', 'x')
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore2.t (i, m, mu) VALUES ('1e3', '1e400', -5)
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore2.t (i, m) VALUES ('99999999999999999999999', '-1e400')
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore2.t (v, dd) VALUES (0x61ff62, '-1e400')
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore2.t (i, b) VALUES (' -7.5 ', NULL)
SHOW WARNINGS
INSERT IGNORE INTO kv_ignore2.t (i, m, dd) VALUES ('-12.5x', '  ', '')
SHOW WARNINGS
SELECT k, i, m, mu, v, LENGTH(b), b = 0x0000, dd FROM kv_ignore2.t ORDER BY k
DROP DATABASE kv_ignore2
CREATE DATABASE kv_replace
CREATE TABLE kv_replace.t (id INT AUTO_INCREMENT PRIMARY KEY, u VARCHAR(3) UNIQUE, v INT UNIQUE, n INT NOT NULL DEFAULT 7)
INSERT INTO kv_replace.t VALUES (1, 'a', 1, 1), (2, 'b', 2, 2), (3, 'c', 3, 3)
REPLACE INTO kv_replace.t VALUES (4, 'd', 4, 4)
REPLACE INTO kv_replace.t VALUES (1, 'a', 10, 10)
REPLACE INTO kv_replace.t VALUES (2, 'c', 4, 20)
SELECT * FROM kv_replace.t ORDER BY id
REPLACE INTO kv_replace.t (u, n) VALUES ('d', 40), ('e', 50), ('e', 51)
REPLACE kv_replace.t SET id = 9, u = 'z'
REPLACE INTO kv_replace.t (u, v) VALUES ('a', NULL), ('y', NULL)
SELECT * FROM kv_replace.t ORDER BY id
SELECT LAST_INSERT_ID()
REPLACE INTO kv_replace.t (u, n) VALUES ('q', NULL)
REPLACE INTO kv_replace.t (u, n) VALUES ('q', 300000000000)
REPLACE INTO kv_replace.t (nope) VALUES (1)
REPLACE IGNORE INTO kv_replace.t VALUES (1)
REPLACE INTO kv_replace.t VALUES (1) ON DUPLICATE KEY UPDATE n = 1
SELECT * FROM kv_replace.t ORDER BY id
DROP DATABASE kv_replace
CREATE DATABASE kv_upsert
CREATE TABLE kv_upsert.t (id INT AUTO_INCREMENT PRIMARY KEY, u VARCHAR(3) UNIQUE, n INT NOT NULL DEFAULT 0, s VARCHAR(5))
INSERT INTO kv_upsert.t (id, u, n) VALUES (1, 'a', 1), (2, 'b', 2)
INSERT INTO kv_upsert.t (u, n) VALUES ('c', 3) ON DUPLICATE KEY UPDATE n = n + 100
INSERT INTO kv_upsert.t (u, n) VALUES ('a', 5) ON DUPLICATE KEY UPDATE n = n + 100, s = CONCAT(u, '!')
INSERT INTO kv_upsert.t (u, n) VALUES ('a', 5) ON DUPLICATE KEY UPDATE n = n
INSERT INTO kv_upsert.t (u, n) VALUES ('b', 7) ON DUPLICATE KEY UPDATE n = VALUES(n)
SHOW WARNINGS
INSERT INTO kv_upsert.t (u, n) VALUES ('b', 8) AS new ON DUPLICATE KEY UPDATE n = new.n + t.n
INSERT INTO kv_upsert.t (u, n) VALUES ('b', 9) AS new (x, y) ON DUPLICATE KEY UPDATE n = y + new.y, s = x
INSERT INTO kv_upsert.t (id, u, n) VALUES (1, 'b', 0) ON DUPLICATE KEY UPDATE n = 42
SELECT * FROM kv_upsert.t ORDER BY id
INSERT INTO kv_upsert.t (u, n) VALUES ('a', 1), ('d', 4), ('a', 2) ON DUPLICATE KEY UPDATE n = n + VALUES(n), s = DEFAULT
SHOW WARNINGS
INSERT INTO kv_upsert.t (u, n) VALUES ('a', 1) ON DUPLICATE KEY UPDATE u = 'b'
INSERT INTO kv_upsert.t (u, n) VALUES ('a', 1) ON DUPLICATE KEY UPDATE id = id + 10, n = id
INSERT INTO kv_upsert.t (u, n) VALUES ('z', 1) ON DUPLICATE KEY UPDATE nope = 1
INSERT INTO kv_upsert.t (u, n) VALUES ('a', 1) ON DUPLICATE KEY UPDATE n = NULL
INSERT IGNORE INTO kv_upsert.t (u, n) VALUES ('a', 1) ON DUPLICATE KEY UPDATE n = NULL, s = 'toolong'
SHOW WARNINGS
INSERT INTO kv_upsert.t (u, n) VALUES ('d', 1) ON DUPLICATE KEY UPDATE s = 'toolong'
SELECT * FROM kv_upsert.t ORDER BY id
SELECT LAST_INSERT_ID()
DROP DATABASE kv_upsert
CREATE DATABASE kv_locking
CREATE TABLE kv_locking.t (id INT PRIMARY KEY, n INT)
INSERT INTO kv_locking.t VALUES (1, 10), (2, 20), (3, 30)
SELECT * FROM kv_locking.t WHERE id > 1 FOR UPDATE
SELECT n FROM kv_locking.t WHERE id = 2 FOR SHARE
SELECT * FROM kv_locking.t ORDER BY n DESC LIMIT 2 LOCK IN SHARE MODE
SELECT * FROM kv_locking.t FOR UPDATE NOWAIT
SELECT * FROM kv_locking.t FOR SHARE SKIP LOCKED
SELECT COUNT(*), MAX(n) FROM kv_locking.t FOR UPDATE
SELECT id FROM kv_locking.t AS x WHERE id = 3 FOR UPDATE OF x
SELECT * FROM kv_locking.t FOR UPDATE OF u
SELECT * FROM kv_locking.t FOR SHARE FOR UPDATE
SELECT * FROM kv_locking.t LOCK IN SHARE MODE NOWAIT
SELECT * FROM kv_locking.t FOR UPDATE NOWAIT SKIP LOCKED
SELECT 1 FOR UPDATE
DROP DATABASE kv_locking
CREATE DATABASE kv_insert_select
CREATE TABLE kv_insert_select.s (a INT, b VARCHAR(5), c DECIMAL(5,2))
CREATE TABLE kv_insert_select.t (id INT AUTO_INCREMENT PRIMARY KEY, a INT, b VARCHAR(3) UNIQUE, c DECIMAL(4,1))
INSERT INTO kv_insert_select.s VALUES (1, 'x', 1.25), (2, 'y', 2.5), (3, 'zzzz', 3), (4, 'x', 999.99)
INSERT INTO kv_insert_select.t (a, b, c) SELECT a, b, c FROM kv_insert_select.s WHERE a <= 2
SHOW WARNINGS
INSERT INTO kv_insert_select.t (a, b) SELECT a, b FROM kv_insert_select.s WHERE a = 3
INSERT IGNORE INTO kv_insert_select.t (a, b, c) SELECT a, b, c FROM kv_insert_select.s
SHOW WARNINGS
INSERT INTO kv_insert_select.t (a) SELECT a, b FROM kv_insert_select.s
INSERT INTO kv_insert_select.t SELECT NULL, a + 10, CONCAT(b, a), c * 2 FROM kv_insert_select.s WHERE a <= 2 ORDER BY a DESC LIMIT 1
INSERT INTO kv_insert_select.t (a, b) SELECT a, b FROM kv_insert_select.s WHERE a = 1 ON DUPLICATE KEY UPDATE a = t.a + 100
INSERT INTO kv_insert_select.t (a, b) SELECT a, b FROM kv_insert_select.s WHERE a = 2 ON DUPLICATE KEY UPDATE a = VALUES(a) + 200
SHOW WARNINGS
REPLACE INTO kv_insert_select.t (a, b) SELECT a, b FROM kv_insert_select.s WHERE a = 2
INSERT INTO kv_insert_select.t (a, b) SELECT COUNT(*), MAX(b) FROM kv_insert_select.s
INSERT INTO kv_insert_select.t (a, b) SELECT a, b FROM kv_insert_select.t WHERE id = 1
INSERT INTO kv_insert_select.t (a, b) SELECT a + 1000, CONCAT('q', id) FROM kv_insert_select.t
INSERT INTO kv_insert_select.t (a, b) SELECT 'nine', 'n'
INSERT INTO kv_insert_select.t (a, b) (SELECT 5, 'p')
SELECT * FROM kv_insert_select.t ORDER BY id
SELECT LAST_INSERT_ID()
DROP DATABASE kv_insert_select
CREATE DATABASE kv_upsert2
CREATE TABLE kv_upsert2.u (id INT PRIMARY KEY, k INT UNIQUE, d INT DEFAULT 5, m INT NOT NULL)
INSERT INTO kv_upsert2.u VALUES (1, 1, 1, 1), (2, 2, 2, 2)
INSERT INTO kv_upsert2.u VALUES (1, 9, 9, 9) ON DUPLICATE KEY UPDATE d = DEFAULT
INSERT INTO kv_upsert2.u VALUES (1, 9, 9, 9) ON DUPLICATE KEY UPDATE m = DEFAULT
INSERT IGNORE INTO kv_upsert2.u VALUES (1, 9, 9, 9) ON DUPLICATE KEY UPDATE m = DEFAULT
SHOW WARNINGS
INSERT IGNORE INTO kv_upsert2.u VALUES (1, 9, 9, 9) ON DUPLICATE KEY UPDATE k = 2
SHOW WARNINGS
INSERT IGNORE INTO kv_upsert2.u (id, k, m) VALUES (3, 3, '1e2x'), (4, 4, '99999999999999999999999999999999999999999999999999999999999999999999999')
SHOW WARNINGS
SELECT * FROM kv_upsert2.u ORDER BY id
DROP DATABASE kv_upsert2
CREATE DATABASE kv_float
CREATE TABLE kv_float.t (k INT PRIMARY KEY, f FLOAT, g FLOAT(7,3))
INSERT INTO kv_float.t (k, f) VALUES (1, 1e7), (2, 1e10), (3, 1e14), (4, 1e15), (5, 0.0001), (6, 1e-5), (7, 1.5e-10), (8, 123456789), (9, -0.000123456789), (10, 999999.5), (11, 1e-15), (12, 1e-16), (13, 0), (14, -1.5), (15, 1.2345678), (16, 3.4028234e38)
INSERT INTO kv_float.t (k, g) VALUES (17, 1.2345678), (18, -1234.5678)
SELECT k, f, g, f + 0, f = 1.2345678 FROM kv_float.t ORDER BY k
SELECT MIN(f), MAX(f), MIN(g) FROM kv_float.t
SELECT * FROM kv_float.t WHERE k = 15
INSERT IGNORE INTO kv_float.t (k, f) VALUES (19, 1e39), (20, -1e39)
SHOW WARNINGS
SELECT f FROM kv_float.t WHERE k >= 19 ORDER BY k
SELECT k, f, MIN(f), f + 0, f = 1.2345678 FROM kv_float.t GROUP BY k ORDER BY k
DROP DATABASE kv_float
CREATE DATABASE kv_grouped
CREATE TABLE kv_grouped.t (k INT PRIMARY KEY, i INT, u INT UNIQUE, c INT, KEY (i))
INSERT INTO kv_grouped.t VALUES (1, 1, 1, 1), (2, 1, 2, 1), (3, 2, NULL, 2)
SELECT k + 0 AS e1, c + 0 AS e2, MIN(c) AS e3 FROM kv_grouped.t GROUP BY k
SELECT i, i + 0 AS e1, MIN(c) + 0 AS e2, MIN(c) AS e3 FROM kv_grouped.t GROUP BY i
SELECT c, c + 0 AS e1, MIN(c) + 0 AS e2, MIN(k) AS e3 FROM kv_grouped.t GROUP BY c
SELECT u, u + 0 AS e1, COUNT(*) AS n FROM kv_grouped.t GROUP BY u ORDER BY u
SELECT c + 0 AS e1, MIN(i) AS e2 FROM kv_grouped.t GROUP BY c, k
SELECT i + 0 AS e1, COUNT(*) AS n FROM kv_grouped.t GROUP BY i, c
SELECT c + 0 AS e1 FROM kv_grouped.t GROUP BY c, i
DROP DATABASE kv_grouped
CREATE DATABASE kv_serial
CREATE TABLE kv_serial.t (id INT PRIMARY KEY, n INT)
INSERT INTO kv_serial.t VALUES (1, 10), (2, 20)
SET @kept_isolation = @@transaction_isolation
SET SESSION transaction_isolation = 'SERIALIZABLE'
SELECT @@transaction_isolation
SELECT * FROM kv_serial.t WHERE id = 1
BEGIN
SELECT COUNT(*), SUM(n) FROM kv_serial.t
UPDATE kv_serial.t SET n = n + 1 WHERE id = 2
SELECT * FROM kv_serial.t ORDER BY id
COMMIT
SET SESSION transaction_isolation = @kept_isolation
DROP DATABASE kv_serial
SELECT CAST('12abc' AS SIGNED), CAST(-1 AS UNSIGNED), CAST(1.5 AS SIGNED), CAST(2.5 AS SIGNED), CAST('-2.5' AS SIGNED), CAST(18446744073709551615 AS SIGNED)
SHOW WARNINGS
SELECT CAST('-1' AS UNSIGNED), CAST('99999999999999999999' AS SIGNED), CAST('-99999999999999999999' AS SIGNED), CAST(1e30 AS SIGNED), CAST(-1.5 AS SIGNED), CAST(X'41' AS UNSIGNED)
SHOW WARNINGS
SELECT CAST(123 AS CHAR), CAST(123 AS CHAR(2)), CAST('abc' AS BINARY), CAST('abc' AS BINARY(5)), CAST(1.5 AS CHAR), CAST(1e3 AS CHAR), CAST(NULL AS CHAR)
SHOW WARNINGS
SELECT CAST(1.23456 AS DECIMAL), CAST(1.23456 AS DECIMAL(5,2)), CAST('9999' AS DECIMAL(3,1)), CAST(1 AS DOUBLE), CAST(1 AS FLOAT), CAST(1 AS FLOAT(30)), CAST(1 AS REAL)
SHOW WARNINGS
SELECT CAST('1.5e2' AS DECIMAL(10,2)), CAST(1.005 AS DECIMAL(5,2)), CAST(-9999 AS DECIMAL(3,1)), CAST(2.5e0 AS DECIMAL), CAST('abc' AS DECIMAL)
SHOW WARNINGS
SELECT CAST(1.5 AS DOUBLE), CAST('1.5x' AS DOUBLE), CAST(NULL AS SIGNED), CAST(1 AS UNSIGNED INTEGER), CONVERT('5', SIGNED INTEGER), CONVERT(123, CHAR)
SHOW WARNINGS
SELECT CONVERT('abc' USING utf8mb4), CONVERT('abc' USING binary), CAST('abc' AS CHAR CHARACTER SET binary), CAST(1 AS CHAR(3) CHARSET utf8mb4)
SELECT CAST(1 AS CHAR(0)), CAST('' AS SIGNED), CAST(' 7 ' AS UNSIGNED)
SHOW WARNINGS
SELECT CAST(1 AS UNSIGNED) + 1, CAST(CAST(-1 AS UNSIGNED) AS SIGNED), CAST('7' AS SIGNED) * 2, CAST(1.25 AS DECIMAL(4,1)) + 0
SELECT CAST(1 AS INT)
SELECT CAST(1 AS DECIMAL(66,0))
SELECT CAST(1 AS DECIMAL(10,11))
SELECT CAST(1 AS DECIMAL(40,31))
SELECT CAST(1 AS FLOAT(54))
SELECT DATE '2024-02-29', TIME '12:34:56', TIMESTAMP '2024-02-29 12:34:56', TIME '12:34:56.789', TIMESTAMP '2024-02-29 12:34:56.5'
SELECT TIME '838:59:59', TIME '-1 02:00:00', DATE '24-2-3', TIMESTAMP '2024-02-03T04:05'
SELECT DATE '2024-02-30'
SELECT TIME '25:61:00'
SELECT TIMESTAMP '2024-02-30 00:00:00'
SELECT TIME '839:00:00'
SELECT CAST('2024-02-29' AS DATE), CAST('2024-02-29 01:02:03.456' AS DATETIME), CAST('2024-02-29 01:02:03.456' AS DATETIME(2)), CAST('25:00:01' AS TIME), CAST('12:00' AS TIME(3))
SELECT CAST(20240229 AS DATE), CAST(20240229123456 AS DATETIME), CAST(123456 AS TIME), CAST('2024-13-01' AS DATE)
SHOW WARNINGS
SELECT CAST('2024-2-3' AS DATE), CAST('2024/02/03' AS DATE), CAST('20240203' AS DATE), CAST('240203' AS DATE), CAST('70-01-01' AS DATE), CAST('69-12-31' AS DATE)
SELECT CAST('2024-02-03T04:05:06' AS DATETIME), CAST('20240203040506' AS DATETIME), CAST('2024-02-03 4:5:6' AS DATETIME), CAST('2024-02-03' AS DATETIME), CAST('2024-02-03 04:05' AS DATETIME)
SELECT CAST('0000-00-00' AS DATE), CAST('2024-00-01' AS DATE), CAST('2024-01-00' AS DATE), CAST('2024-02-29 23:59:59.9999999' AS DATETIME(6)), CAST('2024-12-31 23:59:59.5' AS DATETIME)
SHOW WARNINGS
SELECT CAST('838:59:59' AS TIME), CAST('839:00:00' AS TIME), CAST('-12:00' AS TIME), CAST('1 02:03:04' AS TIME), CAST('0203' AS TIME), CAST('3' AS TIME), CAST('12:60:00' AS TIME)
SHOW WARNINGS
SELECT CAST(20240203 AS DATE), CAST(240203 AS DATE), CAST(20240203040506 AS DATETIME), CAST(203 AS TIME), CAST(-10000 AS TIME), CAST(12.5 AS TIME), CAST(20240203.5 AS DATETIME), CAST(1 AS DATE)
SHOW WARNINGS
SELECT CAST(8390000 AS TIME), CAST(1261 AS TIME), CAST(' 12:00' AS TIME)
SHOW WARNINGS
SELECT CAST(DATE '2024-02-03' AS DATETIME), CAST(TIMESTAMP '2024-02-03 04:05:06.7' AS DATE), CAST(TIMESTAMP '2024-02-03 04:05:06.7' AS TIME), CAST(TIMESTAMP '2024-01-01 10:00:00.25' AS TIME(1))
SELECT CAST('2024-02-03 04:05:06 xyz' AS DATETIME), CAST(' 2024-02-03' AS DATE), CAST('2024-02-03 ' AS DATE)
SHOW WARNINGS
SELECT CAST('abc' AS DATE), CAST('' AS DATETIME), CAST(NULL AS TIME)
SHOW WARNINGS
SELECT DATE '2024-01-01' < DATE '2024-01-02', DATE '2024-01-01' = '2024-01-01', TIMESTAMP '2024-01-01 00:00:00' = DATE '2024-01-01', TIME '10:00:00' > '9:00:00'
SELECT DATE '2024-01-01' < '2024-1-2', DATE '2024-01-01' = 20240101, TIME '10:00:00' = 100000, TIMESTAMP '2024-01-01 10:00:00' > DATE '2024-01-01'
SELECT DATE '2024-01-01' = 'abc'
SHOW WARNINGS
SELECT DATE '2024-01-01' + 0, TIME '10:00:00' + 0, TIMESTAMP '2024-01-01 10:00:00' + 0, TIMESTAMP '2024-01-01 10:00:00.5' + 0
SELECT DATE '2024-01-01' + 1, TIME '10:00:00' * 2, -DATE '2024-01-01', DATE '2024-01-01' AND 1
SELECT CONCAT(DATE '2024-01-01'), LENGTH(TIME '01:02:03'), DATE '2024-01-01' IS NULL
SELECT CAST(1 AS DATETIME(7))
SELECT NOW(), NOW(3), CURRENT_TIMESTAMP, CURRENT_TIMESTAMP(6), LOCALTIME(), LOCALTIMESTAMP, SYSDATE(), SYSDATE(2) FROM DUAL WHERE FALSE
SELECT CURDATE(), CURRENT_DATE, CURRENT_DATE(), CURTIME(), CURTIME(1), CURRENT_TIME, UTC_DATE(), UTC_TIME(), UTC_TIMESTAMP(), UTC_TIMESTAMP(4) FROM DUAL WHERE FALSE
SELECT NOW(7)
SELECT YEAR('2024-02-29 12:34:56.789'), MONTH('2024-02-29'), DAY('2024-02-29'), DAYOFMONTH(DATE '2024-02-29'), HOUR('12:34:56'), MINUTE(TIME '12:34:56'), SECOND('2024-02-29 12:34:56'), MICROSECOND('12:34:56.789')
SELECT DAYOFWEEK('2024-02-29'), WEEKDAY('2024-02-29'), DAYOFYEAR('2024-02-29'), QUARTER('2024-02-29'), DAYNAME('2024-02-29'), MONTHNAME('2024-02-29'), LAST_DAY('2024-02-10'), TO_DAYS('2024-02-29')
SELECT DATE('2024-02-29 12:34:56'), TIME('2024-02-29 12:34:56.5'), DATEDIFF('2024-03-01', '2024-02-28 23:59:59'), HOUR('838:00:00'), YEAR(NULL), MONTH('abc'), YEAR(20240229), HOUR(-1)
SHOW WARNINGS
SELECT YEAR(TIME '10:00:00') IS NOT NULL, DATE(TIME '10:00:00') IS NOT NULL, TIME(DATE '2024-01-01'), DAYNAME('0000-00-00'), LAST_DAY('2024-13-01')
SHOW WARNINGS
SELECT DAYOFWEEK('1970-01-01'), WEEKDAY('2024-03-03'), DAYOFYEAR('2023-12-31'), QUARTER(20241231), LAST_DAY('2024-12-05 10:00'), TO_DAYS(DATE '0001-01-01'), DATEDIFF('2024-01-01', '2024-03-01')
SELECT MONTHNAME(TIMESTAMP '2024-12-01 00:00:00'), DAYNAME(20240303), TIME(123456.25), MICROSECOND(TIME '01:02:03.004005'), SECOND(-10203)
SELECT DATE_ADD(DATE '2024-01-31', INTERVAL 1 MONTH) a, DATE_ADD(DATE '2024-01-01', INTERVAL 1 HOUR) b, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00.5', INTERVAL 1 DAY) c, DATE_ADD(TIME '23:00:00', INTERVAL 2 HOUR) d, DATE_ADD(DATE '2024-01-01', INTERVAL 1 MICROSECOND) g, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL 1.5 SECOND) h, DATE_ADD(DATE '2024-01-01', INTERVAL 1.25 SECOND) i
SELECT DATE_ADD(20240101, INTERVAL 1 DAY) a, DATE_ADD(NULL, INTERVAL 1 DAY) b, DATE_ADD('2024-01-01', INTERVAL 1 DAY) c, DATE_ADD(DATE '2024-01-01', INTERVAL NULL DAY) d, DATE_ADD(CAST('2024-01-01 10:00:00.123' AS DATETIME(3)), INTERVAL 1 DAY) e, DATE_ADD(TIME '10:00:00', INTERVAL '1.5' SECOND_MICROSECOND) f, DATE_SUB(TIME '01:00:00', INTERVAL 2 HOUR) g, DATE_ADD(TIME '838:00:00', INTERVAL 1 HOUR) h
SHOW WARNINGS
SELECT DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1.5' SECOND) a, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1.5' DAY) b, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL 1.5 DAY) c, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL 1.4 DAY) d, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL -1.5 DAY) e, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1x' DAY) g, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL 'x' DAY) h
SHOW WARNINGS
SELECT DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1 2' DAY_HOUR) a, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '-1 2' DAY_HOUR) b, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '5' HOUR_MINUTE) c, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1-2' YEAR_MONTH) d, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL 1 DAY_HOUR) e, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1:2:3:4' DAY_SECOND) f, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1 2:3:4.5' DAY_MICROSECOND) g
SELECT DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1 2 3' DAY_HOUR) i, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1:70' MINUTE_SECOND) k, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL 1.5 HOUR_MINUTE) l, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '1.000005' SECOND_MICROSECOND) m, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL ' -1:2' HOUR_MINUTE) n, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL 'a:b' HOUR_MINUTE) o, DATE_ADD(TIMESTAMP '2024-01-01 00:00:00', INTERVAL '5' SECOND_MICROSECOND) p
SHOW WARNINGS
SELECT DATE_ADD(TIME '10:00:00', INTERVAL '1:2' HOUR_MINUTE) b, DATE_ADD('10:00:00', INTERVAL 1 HOUR) c, DATE_ADD(DATE '0001-01-01', INTERVAL -1 DAY) d, DATE_ADD(DATE '9999-12-31', INTERVAL 1 DAY) e, DATE_ADD(DATE '2024-01-01', INTERVAL 1 QUARTER) f, DATE_ADD(DATE '2024-01-01', INTERVAL 2 WEEK) g, DATE '2024-02-29' + INTERVAL 1 YEAR, DATE '0001-01-01' + INTERVAL -1 YEAR
SHOW WARNINGS
SELECT DATE '2024-01-01' + INTERVAL 1 DAY a, INTERVAL 1 DAY + DATE '2024-01-01' b, DATE '2024-01-01' - INTERVAL 1 DAY c, ADDDATE(DATE '2024-01-01', 3) d, SUBDATE(DATE '2024-01-01', 3) e, ADDDATE(DATE '2024-01-01', INTERVAL 1 MONTH) f, TIMESTAMPADD(MONTH, 1, DATE '2024-01-31') g, TIMESTAMPADD(SQL_TSI_DAY, 1, '2024-01-01') h, TIMESTAMPADD(SECOND, 1.5, DATE '2024-01-01') i, DATE_SUB('2024-01-01', INTERVAL '1 1' DAY_HOUR) j, ADDDATE('2024-01-01', 1.5) k
SELECT INTERVAL 1 + 1 MONTH + DATE '2024-01-31', DATE '2024-01-31' - INTERVAL '1:2' HOUR_MINUTE + 1, '2024-01-01 00:00:00.5' + INTERVAL 1 DAY, '2024-01-01 10:00' - INTERVAL 1 YEAR, DATE '2024-03-31' - INTERVAL 1 MONTH, DATE '2024-01-01' + INTERVAL 1 MONTH - INTERVAL 1 DAY
SELECT TIMESTAMPDIFF(YEAR, '2020-02-29', '2024-02-28') a, TIMESTAMPDIFF(MONTH, '2024-03-31', '2024-01-31') b, TIMESTAMPDIFF(DAY, '2024-01-02', '2024-01-01 23:00') c, TIMESTAMPDIFF(MICROSECOND, '2024-01-01', '2024-01-01 00:00:01.5') d, TIMESTAMPDIFF(QUARTER, '2024-01-01', '2024-12-31') e, TIMESTAMPDIFF(WEEK, '2024-01-01', '2024-01-15') f
SELECT TIMESTAMPDIFF(SECOND, 'x', '2024-01-01') h, TIMESTAMPDIFF(SQL_TSI_MINUTE, NULL, '2024-01-01') i, TIMESTAMPDIFF(MONTH, '2024-01-31 10:00', '2024-02-29 10:00') j, TIMESTAMPDIFF(MONTH, '2024-01-31 10:00', '2024-03-31 09:00') k, TIMESTAMPDIFF(SECOND, 20240101, 20240102) l, TIMESTAMPDIFF(MONTH, '2024-02-29', '2024-01-31') m, TIMESTAMPDIFF(HOUR, DATE '2024-01-01', TIMESTAMP '2023-12-31 01:00:00') n
SHOW WARNINGS
SELECT EXTRACT(YEAR FROM '2024-02-29'), EXTRACT(QUARTER FROM '2024-02-29'), EXTRACT(WEEK FROM '2024-02-29'), EXTRACT(DAY_HOUR FROM '2024-02-29 10:00'), EXTRACT(DAY_MICROSECOND FROM '2024-02-29 10:00'), EXTRACT(HOUR_MICROSECOND FROM '10:00'), EXTRACT(MINUTE_SECOND FROM '10:00'), EXTRACT(MICROSECOND FROM '10:00'), EXTRACT(YEAR_MONTH FROM '2024-02-29')
SELECT EXTRACT(DAY_SECOND FROM '2024-02-29 10:11:12'), EXTRACT(MINUTE_MICROSECOND FROM TIME '-01:02:03.5'), EXTRACT(WEEK FROM '2024-01-01'), EXTRACT(HOUR FROM TIME '-838:00:00'), EXTRACT(DAY_HOUR FROM TIME '30:00:00'), EXTRACT(HOUR_SECOND FROM TIME '-30:01:02'), EXTRACT(WEEK FROM '2023-12-31'), EXTRACT(WEEK FROM '2022-01-01')
SELECT EXTRACT(MONTH FROM '2024-02-29'), EXTRACT(MINUTE FROM '10:00'), EXTRACT(SECOND FROM '10:00'), EXTRACT(HOUR_MINUTE FROM '10:00'), EXTRACT(HOUR_SECOND FROM '10:00'), EXTRACT(DAY_MINUTE FROM '2024-02-29 10:00'), EXTRACT(SECOND_MICROSECOND FROM '10:00:01.25'), EXTRACT(DAY FROM DATE '2024-02-29'), EXTRACT(HOUR FROM '2024-02-29 10:00')
SELECT DATE_FORMAT('2024-02-29 13:04:05.123456', '%a %b %c %D %d %e %f %H %h %I %i %j %k %l %M %m %p %r %S %s %T %U %u %V %v %W %w %X %x %Y %y %% %q') a
SELECT DATE_FORMAT(DATE '2024-02-29', '%Y-%m-%d %H') a, DATE_FORMAT(NULL, '%Y') c, DATE_FORMAT('2024-02-29', NULL) d, DATE_FORMAT('x', '%Y') e, DATE_FORMAT(20240229, '%D') f, DATE_FORMAT('2024-01-01', '%U %u %V %v %X %x') g, DATE_FORMAT('0000-00-00', '%Y') h
SHOW WARNINGS
SELECT DATE_FORMAT('2024-02-29 00:04:05', '%h %I %l %k %p %r') a, DATE_FORMAT('2024-02-29 12:04:05', '%h %p %r') b, DATE_FORMAT('2024-03-01', '%D %D') c, DATE_FORMAT('2024-03-02', '%D') d, DATE_FORMAT('2024-03-03', '%D') e, DATE_FORMAT('2024-03-11', '%D') f, DATE_FORMAT('2024-03-22', '%D') g, DATE_FORMAT('2024-03-23', '%D %j %w') h, DATE_FORMAT('2024-02-29', 'abc%') i, DATE_FORMAT('2024-02-29', '') j
SELECT DATE_FORMAT('2024-02-29', '%M %W %a %b'), DATE_FORMAT('2021-01-03 23:59:59', '%U %u %V %v %X %x %j'), DATE_FORMAT('2026-12-31', '%U %u %V %v %X %x %j'), DATE_FORMAT('1999-01-01', '%V %X %v %x')
SELECT TIME_FORMAT('100:04:05.5', '%H %k %h %I %l %i %S %s %f %p %r %T %%') a, TIME_FORMAT('10:00', '%Y %m %d %a') b, TIME_FORMAT(TIME '-01:02:03', '%H %T') c, TIME_FORMAT('2024-02-29 10:00:00', '%H') d
SELECT TIME_FORMAT('10:00', '%Y %y %m %c %d %e') a, TIME_FORMAT('10:00', '%M') b, TIME_FORMAT('10:00', '%j') c, TIME_FORMAT('10:00', '%U') d, TIME_FORMAT('10:00', '%w') e, TIME_FORMAT(NULL, '%H') f, TIME_FORMAT('x', '%H') g, TIME_FORMAT(DATE '2024-02-29', '%H %d') h, TIME_FORMAT('10:00', '%q%') i
SHOW WARNINGS
SELECT WEEK('2024-01-01') a, WEEK('2024-01-01', 1) b, WEEK('2024-12-31', 2) c, WEEK('2024-12-31', 3) d, WEEK('2021-01-01', 4) e, WEEK('2021-01-01', 5) f, WEEK('2021-01-01', 6) g, WEEK('2021-01-01', 7) h, WEEK('2021-01-01', 8) i, WEEK(NULL) j, WEEK('2024-01-01', NULL) k
SELECT YEARWEEK('2021-01-01') a, YEARWEEK('2021-01-01', 1) b, YEARWEEK('2024-12-31', 3) c, WEEKOFYEAR('2021-01-01') d, WEEKOFYEAR('2024-12-30') e, WEEK('2024-02-29', -1) f, WEEK('x') g, YEARWEEK('2027-01-01', 2), WEEK('2027-01-01', 2), YEARWEEK('2020-12-31', 6)
SHOW WARNINGS
SELECT WEEK('2024-01-01', 0, 1)
SELECT STR_TO_DATE('29/02/2024', '%d/%m/%Y') a, STR_TO_DATE('2024-02-29 13:04:05', '%Y-%m-%d %H:%i:%s') b, STR_TO_DATE('13:04:05', '%H:%i:%s') c, STR_TO_DATE('2024-02-29 13:04:05.5', '%Y-%m-%d %H:%i:%s.%f') d, STR_TO_DATE('Feb 29 2024', '%b %d %Y') e, STR_TO_DATE('x', '%Y') f, STR_TO_DATE(NULL, '%Y') g, STR_TO_DATE('2024', NULL) h, STR_TO_DATE('2024', '%Y') i, STR_TO_DATE('1:04 PM', '%h:%i %p') j
SHOW WARNINGS
SELECT STR_TO_DATE('2024-02-30', '%Y-%m-%d') a, STR_TO_DATE('2024-02-29xyz', '%Y-%m-%d') b, STR_TO_DATE('24-2-9', '%y-%c-%e') c, STR_TO_DATE('Thursday 29th February 2024', '%W %D %M %Y') d, STR_TO_DATE('2024 9 1', '%X %V %w') e, STR_TO_DATE('2024 060', '%Y %j') f, STR_TO_DATE('  2024-02-29', '%Y-%m-%d') g, STR_TO_DATE('2024-02-29', '%Y-%m-%d %H') h, STR_TO_DATE('12:00:00 AM', '%r') i, STR_TO_DATE('2024-02-29', CONCAT('%Y-', '%m-%d')) k
SHOW WARNINGS
SELECT STR_TO_DATE('13:04:05 z', '%H:%i:%s') a, STR_TO_DATE('2024-02-29 13:04 z', '%Y-%m-%d %H:%i') b, STR_TO_DATE('13 PM', '%H %p') c, STR_TO_DATE('2024/02/29', '%Y-%m-%d') d, STR_TO_DATE('2024 9 1', '%Y %V %w') e, STR_TO_DATE('2024 9 1', '%Y %U %w') f, STR_TO_DATE('2024 9 0', '%x %v %w') g, STR_TO_DATE('abc', 'abc') h, STR_TO_DATE('2024.02,29', '%Y%.%m%.%d') i, STR_TO_DATE('25:00', '%H:%i') j
SHOW WARNINGS
SELECT STR_TO_DATE('2024 jan 5 MONDAY', '%Y %b %e %W') a, STR_TO_DATE('99-12-31 11:59:59 pm', '%y-%m-%d %r') b, STR_TO_DATE('2024-02-29 13:04:05.1234567', '%Y-%m-%d %T.%f') c, STR_TO_DATE('29', '%d') d, STR_TO_DATE('2024-02-29', '%Y-%m-%d%%') e, STR_TO_DATE(20240229, '%Y%m%d') f, STR_TO_DATE('2024-02-29', 5) g, STR_TO_DATE('0:30', '%l:%i') k
SHOW WARNINGS
SELECT UNIX_TIMESTAMP('2024-02-29 12:34:56') a, UNIX_TIMESTAMP('2024-02-29 12:34:56.5') b, UNIX_TIMESTAMP(DATE '2024-02-29') c, UNIX_TIMESTAMP('1969-12-31') d, UNIX_TIMESTAMP('x') e, UNIX_TIMESTAMP(NULL) f, UNIX_TIMESTAMP(20240229) g, UNIX_TIMESTAMP(CAST('2024-01-01 00:00:00.123' AS DATETIME(3))) h, UNIX_TIMESTAMP('3001-01-19 03:14:08') i, UNIX_TIMESTAMP('3001-01-19 03:14:07') k
SHOW WARNINGS
SELECT FROM_UNIXTIME(1709210096) a, FROM_UNIXTIME(1709210096.5) b, FROM_UNIXTIME(1709210096, '%Y %D') c, FROM_UNIXTIME(-1) d, FROM_UNIXTIME(NULL) e, FROM_UNIXTIME('1709210096') f, FROM_UNIXTIME(1709210096.123456789) g, FROM_UNIXTIME(32536771199) h, FROM_UNIXTIME(32536771200) i, FROM_UNIXTIME(1e9) j, FROM_UNIXTIME(0)
SHOW WARNINGS
SELECT ADDTIME('2024-02-29 23:00:00', '01:30:00') a, ADDTIME(TIME '10:00:00', '1:00:00.5') b, ADDTIME(TIMESTAMP '2024-02-29 23:00:00', TIME '02:00:00') c, SUBTIME('10:00:00', '11:00:00') d, ADDTIME('x', '1:00') e, ADDTIME('10:00', NULL) f, ADDTIME('838:00:00', '1:00:00') g, ADDTIME('2024-02-29 23:00:00', '1 01:00:00') i
SHOW WARNINGS
SELECT ADDTIME(DATE '2024-02-29', '01:00:00') a, ADDTIME('10:00', DATE '2024-02-29') b, ADDTIME('10:00', TIMESTAMP '2024-02-29 00:00:00') c, SUBTIME(TIME '-01:00:00', '00:30') d, ADDTIME('10:00:00', '00:00:00.25') e, ADDTIME('2024-02-29 23:59:59', '0:0:0.5') f
SELECT TIMEDIFF('2024-02-29 12:00:00', '2024-02-28 10:00:00') a, TIMEDIFF('10:00:00', '12:30:00.5') b, TIMEDIFF(TIME '10:00:00', TIME '01:00:00') e, TIMEDIFF(NULL, '1:00') f, TIMEDIFF('2024-03-31 00:00:00', '2024-01-01 00:00:00') g, TIMEDIFF('2024-02-29 10:00', '09:00') h, TIMEDIFF(TIMESTAMP '2024-03-31 00:00:00', TIME '01:00:00') i, TIMEDIFF(DATE '2024-03-31', DATE '2024-03-30') j
SHOW WARNINGS
SELECT MAKEDATE(2024, 60) a, MAKEDATE(2024, 0) b, MAKEDATE(24, 1) c, MAKEDATE(2024, 400) d, MAKETIME(10, 30, 15.5) e, MAKETIME(900, 0, 0) f, MAKETIME(-1, 2, 3) g, MAKETIME(1, 60, 0) h, SEC_TO_TIME(3661) i, SEC_TO_TIME(-3661.25) j, SEC_TO_TIME(99999999) k, TIME_TO_SEC('01:01:01') l, TIME_TO_SEC('-01:00:00.5') m, FROM_DAYS(739310) n, FROM_DAYS(365) o, TO_SECONDS('2024-02-29 00:00:01') p
SHOW WARNINGS
SELECT MAKEDATE(NULL, 1), MAKETIME(1, 2, NULL), SEC_TO_TIME('3661'), SEC_TO_TIME(NULL), TIME_TO_SEC(NULL), FROM_DAYS(NULL), TO_SECONDS(NULL), MAKETIME(1, 2, 60), MAKETIME(0, 0, 59.999999), TO_SECONDS(DATE '0001-01-01')
SELECT EXTRACT(HOUR FROM 'abc'), EXTRACT(YEAR FROM 'abc'), EXTRACT(YEAR FROM NULL), EXTRACT(YEAR FROM 20240229), EXTRACT(HOUR FROM 101112), EXTRACT(DAY_HOUR FROM DATE '2024-02-29'), EXTRACT(DAY_SECOND FROM TIMESTAMP '2024-02-29 01:02:03')
SHOW WARNINGS
CREATE DATABASE kv_temporal
CREATE TABLE kv_temporal.t (id INT PRIMARY KEY, d DATE, t TIME(3), dt DATETIME(6), ts TIMESTAMP NULL, y YEAR, dn DATE NOT NULL DEFAULT '2000-01-01')
INSERT INTO kv_temporal.t (id, d, t, dt, ts, y) VALUES (1, '2024-02-29', '12:34:56.7891', '2024-02-29 12:34:56.1234567', '2024-02-29 12:34:56', 2024)
INSERT INTO kv_temporal.t (id, d, t, dt, ts, y) VALUES (2, 20240301, 123456, 20240301123456, NULL, 24)
SELECT d + INTERVAL 1 YEAR, d + INTERVAL 1 HOUR, t + INTERVAL 1 SECOND, t + INTERVAL 1 MICROSECOND, dt - INTERVAL 1 MICROSECOND, ts + INTERVAL 1 DAY, y + INTERVAL 1 DAY, EXTRACT(HOUR_MICROSECOND FROM dt), TIMESTAMPDIFF(SECOND, d, dt) FROM kv_temporal.t ORDER BY id
SELECT WEEK(d), WEEK(dt, 5), DATE_FORMAT(dt, '%W %e %M %Y %r'), TIME_FORMAT(t, '%l:%i %p'), YEARWEEK(d, 1), DATE_FORMAT(d, CONCAT('%', 'Y')) FROM kv_temporal.t ORDER BY id
SELECT UNIX_TIMESTAMP(dt), UNIX_TIMESTAMP(ts), FROM_UNIXTIME(UNIX_TIMESTAMP(d)), ADDTIME(dt, t), ADDTIME(t, '1:00'), TIMEDIFF(dt, d), TIME_TO_SEC(t), SEC_TO_TIME(id * 3600), TO_SECONDS(d), MAKEDATE(YEAR(d), id) FROM kv_temporal.t ORDER BY id
SELECT STR_TO_DATE(DATE_FORMAT(dt, '%d.%m.%Y %T'), '%d.%m.%Y %T'), STR_TO_DATE(CONCAT(d), CONCAT('%Y-%m-', 'X', IF(id = 1, '%d', '%e'))) FROM kv_temporal.t ORDER BY id
INSERT INTO kv_temporal.t (id, d, t, dt, ts, y) VALUES (3, '24-3-2', '-1 02:00', '2024-03-02T01:02:03', '2024-03-02 01:02:03', '1999')
SELECT * FROM kv_temporal.t ORDER BY id
INSERT INTO kv_temporal.t (id, d) VALUES (4, '2024-02-30')
INSERT INTO kv_temporal.t (id, d) VALUES (5, '0000-00-00')
INSERT INTO kv_temporal.t (id, t) VALUES (6, '839:00:00')
INSERT INTO kv_temporal.t (id, y) VALUES (7, 1900)
INSERT INTO kv_temporal.t (id, ts) VALUES (8, '1969-12-31 23:59:59')
INSERT INTO kv_temporal.t (id, d) VALUES (9, '2024-02-03 04:05:06')
SHOW WARNINGS
SELECT d + 0, t + 0, dt + 0, y + 0, d = '2024-02-29', y = 2024 FROM kv_temporal.t WHERE id = 1
SELECT id, d FROM kv_temporal.t WHERE d > '2024-03-01' ORDER BY d
SELECT id FROM kv_temporal.t WHERE dt BETWEEN '2024-03-01' AND '2024-03-02 23:00' ORDER BY id
UPDATE kv_temporal.t SET d = '2025-01-01', y = '0' WHERE id = 2
SELECT d, y FROM kv_temporal.t WHERE id = 2
INSERT INTO kv_temporal.t (id, y) VALUES (10, 'abc')
INSERT IGNORE INTO kv_temporal.t (id, d, t, y) VALUES (11, '2024-02-30', '839:00:00', 1900)
SHOW WARNINGS
SELECT id, d, t, y FROM kv_temporal.t WHERE id = 11
SELECT DATA_TYPE, COLUMN_TYPE, DATETIME_PRECISION, COLUMN_DEFAULT FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'kv_temporal' ORDER BY ORDINAL_POSITION
INSERT INTO kv_temporal.t (id, d, t, dt, y) VALUES (12, TIMESTAMP '2024-05-06 07:08:09', TIMESTAMP '2024-05-06 07:08:09.5', DATE '2024-05-06', 2024.4)
SHOW WARNINGS
INSERT INTO kv_temporal.t (id, t, dt, y) VALUES (13, TIME '01:02:03', CAST('2024-05-06 07:08:09.1234567' AS DATETIME(6)), 99e0)
SELECT id, d, t, dt, y FROM kv_temporal.t WHERE id IN (12, 13) ORDER BY id
CREATE TABLE kv_temporal.c (id INT PRIMARY KEY, a DATETIME DEFAULT CURRENT_TIMESTAMP, b TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3), c DATETIME ON UPDATE NOW(), e TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP, n INT)
SELECT * FROM kv_temporal.c WHERE FALSE
SELECT COLUMN_NAME, COLUMN_DEFAULT, EXTRA FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'kv_temporal' AND TABLE_NAME = 'c' ORDER BY ORDINAL_POSITION
INSERT INTO kv_temporal.c (id, n) VALUES (1, 1)
SELECT a IS NOT NULL, b IS NOT NULL, c IS NULL, e IS NOT NULL, LENGTH(b) FROM kv_temporal.c
UPDATE kv_temporal.c SET n = 2
SELECT c IS NOT NULL FROM kv_temporal.c
UPDATE kv_temporal.c SET n = 2
SELECT ROW_COUNT()
CREATE TABLE kv_temporal.bad3 (a DATETIME(3) DEFAULT CURRENT_TIMESTAMP)
CREATE TABLE kv_temporal.bad4 (a DATE DEFAULT CURRENT_TIMESTAMP)
CREATE TABLE kv_temporal.bad5 (a INT ON UPDATE CURRENT_TIMESTAMP)
CREATE TABLE kv_temporal.bad6 (a DATETIME ON UPDATE CURRENT_TIMESTAMP(2))
CREATE TABLE kv_temporal.z (id INT PRIMARY KEY, ts TIMESTAMP NULL, dt DATETIME)
SET time_zone = '+10:00'
INSERT INTO kv_temporal.z VALUES (1, '2024-01-01 10:00:00', '2024-01-01 10:00:00')
INSERT INTO kv_temporal.z VALUES (2, '1970-01-01 10:00:01', NULL)
INSERT INTO kv_temporal.z VALUES (3, '1970-01-01 10:00:00', NULL)
SELECT id, ts, dt FROM kv_temporal.z ORDER BY id
SET time_zone = '+00:00'
SELECT id, ts, dt, ts = '2024-01-01 00:00:00', ts + 0 FROM kv_temporal.z ORDER BY id
SET time_zone = '-05:30'
SELECT id, ts FROM kv_temporal.z WHERE ts > '2023-12-31 18:00:00' ORDER BY id
SET time_zone = '+14:00'
SELECT ts FROM kv_temporal.z WHERE id = 1
SET time_zone = '+14:01'
SET time_zone = '-13:59'
SET time_zone = '-14:00'
SET time_zone = '+5:30'
SELECT @@time_zone
SET time_zone = 'SYSTEM'
CREATE TABLE kv_temporal.bad1 (a TIME(7))
CREATE TABLE kv_temporal.bad2 (a YEAR(2))
CREATE TABLE kv_temporal.k (t TIME PRIMARY KEY)
INSERT INTO kv_temporal.k VALUES ('-01:00:00'), ('100:00:00'), ('09:00:00'), ('-100:00:00')
SELECT t FROM kv_temporal.k
DROP DATABASE kv_temporal
SELECT CURTIME(7)
SELECT NOW() = SYSDATE() OR NOW() < SYSDATE(), CAST(NOW() AS DATE) = CURDATE(), NOW() = CURRENT_TIMESTAMP, LENGTH(NOW(3)), NOW() + 0 > 20240101000000
CREATE DATABASE kv_join
CREATE TABLE kv_join.a (id INT PRIMARY KEY, x VARCHAR(10) NOT NULL, n INT)
CREATE TABLE kv_join.b (id INT PRIMARY KEY, a_id INT NOT NULL, y VARCHAR(5))
INSERT INTO kv_join.a VALUES (1, 'one', 10), (2, 'two', NULL), (3, 'three', 30)
INSERT INTO kv_join.b VALUES (10, 1, 'p'), (11, 1, 'q'), (12, 3, NULL), (13, 9, 'z')
SELECT * FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY b.id
SELECT * FROM kv_join.b, kv_join.a WHERE a.id = b.a_id ORDER BY b.id DESC
SELECT a.x, b.y FROM kv_join.b INNER JOIN kv_join.a ON a.id = b.a_id AND b.y IS NOT NULL ORDER BY b.y
SELECT a.id, b.id FROM kv_join.a CROSS JOIN kv_join.b WHERE b.id = 13 ORDER BY a.id
SELECT p.x, q.x FROM kv_join.a p JOIN kv_join.a q ON q.id = p.id + 1 ORDER BY p.id
SELECT a.id + 1, b.a_id * 2, CONCAT(x, y) FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY b.id
SELECT kv_join.a.id, a.x FROM kv_join.a, kv_join.b ORDER BY 1, b.id LIMIT 3
SELECT a.id, b.id, c.n FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id JOIN kv_join.a AS c ON c.n = a.n ORDER BY b.id
SELECT a.id FROM kv_join.a JOIN (kv_join.b JOIN kv_join.a AS c ON c.id = b.a_id) ON a.id = b.a_id ORDER BY b.id
SELECT DISTINCT a.x FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id ORDER BY a.x
SELECT id FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id
SELECT 1 FROM kv_join.a JOIN kv_join.b ON 1 WHERE id = 1
SELECT 1 FROM kv_join.a JOIN kv_join.b ON id = 1
SELECT x FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id ORDER BY id
SELECT 1 FROM kv_join.a, kv_join.b JOIN kv_join.a AS c ON a.id = c.id
SELECT 1 FROM kv_join.a JOIN kv_join.a
SELECT zz.* FROM kv_join.a JOIN kv_join.b ON 1
CREATE TABLE kv_join.c (k INT, m INT)
INSERT INTO kv_join.c VALUES (1, 2)
SELECT a.id, b.id, a.id + 1, b.id + 1 FROM kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id ORDER BY a.id, b.id
SELECT a.id, b.id FROM kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id ORDER BY a.x, b.id
SELECT a.id, b.id FROM kv_join.b RIGHT JOIN kv_join.a ON a.id = b.a_id ORDER BY a.id, b.id
SELECT a.id, b.id FROM kv_join.a RIGHT JOIN kv_join.b ON a.id = b.a_id ORDER BY b.id
SELECT a.x, b.y FROM kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id AND b.y = 'q' ORDER BY a.id
SELECT * FROM kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id WHERE b.id IS NULL ORDER BY a.id
SELECT a.x, COALESCE(b.y, '-') FROM kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id AND b.y = 'q' ORDER BY a.id
SELECT * FROM kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id LEFT JOIN kv_join.c ON c.k = b.a_id ORDER BY a.id, b.id
SELECT * FROM kv_join.a LEFT JOIN kv_join.b USING (id) ORDER BY id
SELECT * FROM kv_join.b RIGHT JOIN kv_join.a USING (id) ORDER BY id
SELECT id, a.id, b.id FROM kv_join.a LEFT JOIN kv_join.b USING (id) ORDER BY 1
SELECT * FROM kv_join.a NATURAL JOIN kv_join.a AS d ORDER BY id
SELECT * FROM kv_join.a NATURAL JOIN kv_join.c ORDER BY id
SELECT * FROM kv_join.a NATURAL LEFT JOIN kv_join.b ORDER BY id
SELECT * FROM kv_join.a JOIN kv_join.b USING (zz)
SELECT * FROM kv_join.a JOIN kv_join.b USING (a_id)
SELECT * FROM kv_join.a, kv_join.b JOIN kv_join.c USING (id)
SELECT a.id, COUNT(b.id), COUNT(*), MAX(b.y), SUM(b.id) FROM kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id GROUP BY a.id ORDER BY a.id
SELECT a.x, COUNT(*) FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id GROUP BY a.x ORDER BY a.x
SELECT COUNT(*), COUNT(b.y), MIN(a.x) FROM kv_join.a, kv_join.b
SELECT a.id, COUNT(*) FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id GROUP BY a.id HAVING COUNT(*) > 1
SELECT b.a_id, a.x FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id GROUP BY b.a_id ORDER BY 1
SELECT b.id, a.x, COUNT(*) FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id GROUP BY b.id ORDER BY 1
SELECT id, COUNT(*) FROM kv_join.a JOIN kv_join.b USING (id) GROUP BY id
SELECT DISTINCT a.x FROM kv_join.a JOIN kv_join.b ON 1 GROUP BY a.x, b.y ORDER BY a.x
SELECT b.a_id, b.y FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id GROUP BY b.a_id
SELECT a.x, b.y FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id GROUP BY a.x
SELECT id, COUNT(*) FROM kv_join.a JOIN kv_join.b ON 1 GROUP BY id
SELECT a.id, b.id FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY b.id FOR UPDATE
SELECT a.id, b.id FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id FOR UPDATE OF zz
SELECT a.id, b.id FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id FOR UPDATE OF a FOR SHARE OF a
SELECT a.id, b.id FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY b.id FOR UPDATE OF a FOR SHARE OF b
SELECT t.id FROM kv_join.a AS t JOIN kv_join.b ON t.id = b.a_id FOR UPDATE OF a
SELECT a.id, COUNT(*) FROM kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id GROUP BY a.id ORDER BY a.id FOR SHARE
SELECT a.n * 9223372036854775807 FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id
SELECT a.id, a.n * 9223372036854775807 FROM kv_join.a AS a JOIN kv_join.b ON a.id = b.a_id GROUP BY a.id
SELECT DISTINCT a.x FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id ORDER BY b.y
SELECT a.x AS w FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY w
SELECT a.x FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY 7
SELECT a.x FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY nope
SELECT b.*, COUNT(*) FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id GROUP BY b.id ORDER BY b.id
SELECT kv_join.b.*, a.x FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY 1
SELECT zz.*, COUNT(*) FROM kv_join.b JOIN kv_join.a ON 1 GROUP BY b.id
SELECT * FROM kv_join.b JOIN kv_nodb.a ON 1
SELECT DISTINCT a.x, a.n + 1 FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id ORDER BY a.x
SELECT a.x FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id ORDER BY 99999999999999999999
SELECT x FROM kv_join.a ORDER BY 99999999999999999999
SELECT 18446744073709551615, 18446744073709551616, 99999999999999999999
SELECT *, COUNT(*) FROM kv_join.b JOIN kv_join.a ON a.id = b.a_id GROUP BY b.id ORDER BY b.id
SELECT DISTINCT a.x FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id ORDER BY CONCAT(a.x, b.y)
SELECT COUNT(*) FROM kv_join.a JOIN kv_join.b ON 1 WHERE id = 1
UPDATE kv_join.a JOIN kv_join.b ON a.id = b.a_id SET a.n = 1 WHERE a.x = 0
UPDATE kv_join.a JOIN kv_join.b ON a.id = b.a_id SET a.n = a.n + 1, b.y = 'u'
SELECT ROW_COUNT()
SELECT * FROM kv_join.a ORDER BY id
SELECT * FROM kv_join.b ORDER BY id
UPDATE kv_join.a, kv_join.b SET a.n = b.id WHERE a.id = b.a_id AND b.id = 12
SELECT ROW_COUNT()
UPDATE kv_join.a LEFT JOIN kv_join.b ON a.id = b.a_id SET a.x = CONCAT(a.x, '!') WHERE b.id IS NULL
SELECT ROW_COUNT()
UPDATE kv_join.a JOIN kv_join.b ON a.id = b.a_id SET n = 1, a.x = CONCAT(n, '')
SELECT * FROM kv_join.a ORDER BY id
UPDATE kv_join.a JOIN kv_join.b ON a.id = b.a_id SET id = 1
UPDATE kv_join.a JOIN kv_join.b ON a.id = b.a_id SET a.n = 1 ORDER BY a.id
UPDATE kv_join.a JOIN kv_join.b ON a.id = b.a_id SET a.n = 1 LIMIT 1
UPDATE kv_join.a JOIN kv_join.b ON a.id = b.a_id SET zz.n = 1
UPDATE kv_join.a JOIN kv_join.b ON a.id = b.a_id SET b.id = 13
SELECT * FROM kv_join.b ORDER BY id
DELETE kv_join.a, kv_join.b FROM kv_join.a JOIN kv_join.b ON a.id = b.a_id WHERE a.id = 3
SELECT ROW_COUNT()
DELETE FROM kv_join.b USING kv_join.a JOIN kv_join.b ON a.id = b.a_id WHERE a.id = 1
SELECT ROW_COUNT()
DELETE kv_join.b.* FROM kv_join.b LEFT JOIN kv_join.a ON a.id = b.a_id WHERE a.id IS NULL
SELECT ROW_COUNT()
DELETE kv_join.zz FROM kv_join.a JOIN kv_join.b ON 1
DELETE a FROM kv_join.a JOIN kv_join.b ON 1 ORDER BY a.id
SELECT * FROM kv_join.a ORDER BY id
SELECT * FROM kv_join.b ORDER BY id
DROP DATABASE kv_join
SELECT LPAD('ab',5,'xy'), RPAD('ab',5,'xy'), LPAD('abcdef',3,'x'), LPAD('ab',5,''), LPAD(NULL,3,'x'), LPAD('a',3,NULL), RPAD(1,3,0), LPAD('ab', 0, 'x'), RPAD('é',3,'ü'), LPAD('ab',-1,'x')
SELECT REPEAT('ab',3), REPEAT('ab',0), REPEAT('ab',-1), REPEAT(NULL,2), REPEAT(1,2), REVERSE('abc'), REVERSE('héllo'), REVERSE(NULL), REVERSE(123), REVERSE(_binary'ab')
SELECT REPEAT('ab', 40000000)
SHOW WARNINGS
SELECT LOCATE('a')
SELECT REPLACE('a','b')
SELECT TRIM()
CREATE DATABASE kv_strings2
USE kv_strings2
CREATE TABLE f (i INT, v VARCHAR(10), c CHAR(3) COLLATE utf8mb4_bin, x TEXT, vb VARBINARY(5))
INSERT INTO f VALUES (1, 'abc', 'x', 'tt', 'zz'), (NULL, NULL, NULL, NULL, NULL)
SELECT LPAD(v, i, 'x'), REPEAT(v, i), TRIM(c), REPLACE(v, 'b', c), REVERSE(x), LOCATE(c, v), RPAD(c, 4, v), TRIM(BOTH c FROM v), REPEAT(vb, 2), LPAD(v, 3, vb) FROM f
SELECT REPLACE(v,'b',v), REPLACE(v,v,'xyz'), REPLACE(v,'bc',c), REPLACE(c,'b',v), REPLACE('abc','b',c), REPLACE(v,'b',''), REPLACE(v, c, 'x') FROM f
DROP DATABASE kv_strings2
SELECT ABS(-5), ABS(-2.50), ABS(-1.5e0), ABS(NULL), ABS('-3'), ABS(18446744073709551615), ABS(0x41), ABS(-9223372036854775807)
SELECT ABS(-9223372036854775808)
SELECT SIGN(-5), SIGN(0), SIGN(2.5), SIGN(-1e0), SIGN(NULL), SIGN('x')
SHOW WARNINGS
SELECT FLOOR(1.5), FLOOR(-1.5), FLOOR(1.5e0), FLOOR(7), FLOOR(NULL), FLOOR('2.7'), FLOOR(12345678901234567.5), FLOOR(-12345678901234567.5), FLOOR(123456789012345678.5), FLOOR(18446744073709551615)
SELECT CEILING(1.2), CEIL(-1.2), CEILING(1.2e0), CEILING(5), CEILING(99.99), CEILING(0x41)
SELECT ROUND(1.5), ROUND(2.5), ROUND(-1.5), ROUND(-0.4), ROUND(1.25,1), ROUND(123.456,-1), ROUND(1.234,5), ROUND(9999999999999999.5), ROUND(1.5,NULL)
SELECT ROUND(1.5e0), ROUND(2.5e0), ROUND(1.2345e0, 2), ROUND('1.5'), ROUND(NULL), ROUND(15), ROUND(-15), ROUND(15,-1), ROUND(18446744073709551615)
SELECT TRUNCATE(1.99,1), TRUNCATE(-1.99,1), TRUNCATE(1.99e0,1), TRUNCATE(199,-2), TRUNCATE(1.5,0), TRUNCATE(1.5,3), TRUNCATE(NULL,1), TRUNCATE(1.5,NULL)
SELECT MOD(7,3), MOD(-7,3), MOD(7.5,2), MOD(7,0), MOD(7e0,2), 7 MOD 3
SELECT MOD(1)
CREATE DATABASE kv_numbers
USE kv_numbers
CREATE TABLE f (i INT, u INT UNSIGNED NOT NULL, d DECIMAL(6,2), t TINYINT, db DOUBLE)
INSERT INTO f VALUES (1, 2, 4.5, 7, 1.25), (-16, 5, -2.55, NULL, -0.5)
SELECT ABS(i), ABS(u), ABS(d), ABS(t), FLOOR(d), FLOOR(i), FLOOR(db), ROUND(d), ROUND(d, 1), ROUND(d, i), ROUND(i, i), TRUNCATE(d, 1), SIGN(d), CEILING(u), ROUND(i, -1), ROUND(u) FROM f
DROP DATABASE kv_numbers
SELECT @@character_set_database, @@collation_database, @@session.character_set_database, @@global.collation_database
select @@character_set_client, @@character_set_connection, @@character_set_server, @@character_set_database limit 1
CREATE DATABASE kv_dbcs COLLATE utf8mb4_bin
CREATE DATABASE kv_dbcs2 CHARACTER SET binary
USE kv_dbcs2
SELECT @@character_set_database, @@collation_database
USE kv_dbcs
SELECT @@character_set_database, @@collation_database
SET collation_database = 'utf8mb4_0900_as_cs'
SELECT @@character_set_database, @@collation_database
SET character_set_database = 'binary'
SELECT @@character_set_database, @@collation_database
SET collation_database = DEFAULT
SELECT @@character_set_database, @@collation_database
SET character_set_database = 'UTF8MB4'
SELECT @@character_set_database, @@collation_database
SET collation_database = 'utf8mb4_general_ci', character_set_database = 'binary'
SELECT @@character_set_database, @@collation_database
SET character_set_database = DEFAULT
SELECT @@character_set_database, @@collation_database
SET character_set_database = 'nope'
SET collation_database = 'utf8mb4_nope'
SET collation_database = NULL
SET character_set_database = NULL
SET collation_connection = NULL
DROP DATABASE kv_dbcs2
SELECT @@character_set_database, @@collation_database
DROP DATABASE kv_dbcs
SELECT @@character_set_database, @@collation_database
# What connectors send as they connect, logged on the reference server
# (2026-11-19). Connector/J 9.1.0's first query, less @@performance_schema
# (an allowed difference):
/* mysql-connector-j-9.1.0 (Revision: cf2917ea44ae2e43a4514a33771035aa99de73bf) */SELECT  @@session.auto_increment_increment AS auto_increment_increment, @@character_set_client AS character_set_client, @@character_set_connection AS character_set_connection, @@character_set_results AS character_set_results, @@character_set_server AS character_set_server, @@collation_server AS collation_server, @@collation_connection AS collation_connection, @@init_connect AS init_connect, @@interactive_timeout AS interactive_timeout, @@license AS license, @@lower_case_table_names AS lower_case_table_names, @@max_allowed_packet AS max_allowed_packet, @@net_write_timeout AS net_write_timeout, @@sql_mode AS sql_mode, @@system_time_zone AS system_time_zone, @@time_zone AS time_zone, @@transaction_isolation AS transaction_isolation, @@wait_timeout AS wait_timeout
SET character_set_results = NULL
SET autocommit=1
SELECT 1
SET autocommit=0
COMMIT
SET autocommit=1
# mysql-connector-python 26.10.0 (C and pure), and PyMySQL 1.2.4.
SET NAMES 'utf8mb4' COLLATE 'utf8mb4_0900_ai_ci'
set autocommit=0
SELECT @@autocommit, @@character_set_results, @@collation_connection
commit
SET @@session.autocommit = OFF
COMMIT
SET NAMES utf8mb4
SET AUTOCOMMIT = 0
SELECT @@autocommit
COMMIT
# go-sql-driver/mysql 1.8.1.
START TRANSACTION
COMMIT
SET autocommit = 1
SELECT @@autocommit
# Result metadata is reported in character_set_results' collation, converted
# from each column's own; nothing is converted when it is NULL.
CREATE DATABASE kv_meta
USE kv_meta
CREATE TABLE t (v VARCHAR(10), b VARCHAR(5) COLLATE utf8mb4_bin, c CHAR(3), x TEXT, vb VARBINARY(4), m VARCHAR(4) CHARACTER SET utf8mb3, i INT)
INSERT INTO t VALUES ('a', 'b', 'c', 'x', 'y', 'm', 1)
SET character_set_results = NULL
SELECT v, b, c, x, vb, m, i, CONCAT(v, m), CONCAT(v, @@sql_mode), 'é' FROM t
SELECT 'a', @@sql_mode, @@character_set_results, @@collation_connection, DATABASE(), CONCAT('a', @@sql_mode)
SELECT 1/0
SHOW WARNINGS
SHOW TABLES
SHOW DATABASES LIKE 'kv_meta'
SET character_set_results = binary
SELECT v, b, c, x, vb, m, i, CONCAT(v, m), 'é' FROM t
SELECT 'a', @@sql_mode, DATABASE(), CONCAT('a', @@sql_mode)
SELECT 1/0
SHOW WARNINGS
SHOW TABLES
SET character_set_results = utf8mb4
SELECT v, b, c, x, vb, m, i, CONCAT(v, m), 'é' FROM t
SHOW TABLES
SET collation_connection = 'utf8mb4_bin'
SELECT 'a', @@sql_mode
SET NAMES utf8mb4 COLLATE utf8mb4_bin
SET collation_connection = 'utf8mb4_0900_ai_ci'
SELECT 'a', @@sql_mode
SET NAMES utf8mb4
DROP DATABASE kv_meta
SELECT ?
SELECT 1 LIMIT ?
SET NAMES utf8mb4 COLLATE utf8mb4_general_ci
SELECT UPPER('aé'), UPPER('ǹ'), UPPER('ƀ'), LOWER('Ⱥ'), UCASE('ı'), LCASE('İ')
SET NAMES utf8mb4
SELECT UPPER('ƀ'), UPPER('ǹ'), LOWER('Ƀ'), UPPER('aé'), UCASE('ς'), LCASE('Σ')
CREATE DATABASE kv_case
USE kv_case
CREATE TABLE c (g VARCHAR(5) COLLATE utf8mb4_general_ci, b VARCHAR(5) COLLATE utf8mb4_bin, m VARCHAR(5) CHARACTER SET utf8mb3, n VARCHAR(5), x TEXT)
INSERT INTO c VALUES ('ƀǹ', 'ƀǹ', 'ǹa', 'ƀǹ', 'ƀa')
SELECT UPPER(g), UPPER(b), UPPER(m), UPPER(n), LOWER(UPPER(n)), UPPER(x), LOWER(b) FROM c
DROP DATABASE kv_case
CREATE DATABASE kv_hv
USE kv_hv
CREATE TABLE t (a INT, b VARCHAR(5))
INSERT INTO t VALUES (1, 'x'), (2, 'y'), (3, 'x')
SELECT a, b AS kind FROM t HAVING kind = 'x' ORDER BY a
SELECT a, CASE WHEN b = 'x' THEN 'TABLE' ELSE 'VIEW' END AS t_type FROM t HAVING t_type IN ('TABLE', null, null) ORDER BY t_type, a
SELECT a FROM t HAVING a > 1
SELECT a FROM t HAVING b = 'x'
SELECT a AS x FROM t WHERE a > 0 HAVING x < 3 ORDER BY x DESC
SELECT 1 AS one HAVING one = 1
SELECT 1 AS one HAVING one = 2
SELECT a, COUNT(*) FROM t HAVING a = 1
DROP DATABASE kv_hv
# information_schema: SCHEMATA, TABLES and COLUMNS over the catalog, and
# Connector/J 9.1.0's getTables and getColumns queries as it sends them.
# TABLES' storage figures (TABLE_ROWS, AUTO_INCREMENT, the byte sizes) are
# MySQL's cached statistics, and CREATE_TIME, UPDATE_TIME and CHECK_TIME are
# refused until temporal types: both left out.
CREATE DATABASE kv_isc
USE kv_isc
CREATE TABLE t (a INT NOT NULL AUTO_INCREMENT, b TINYINT, c SMALLINT UNSIGNED, d MEDIUMINT DEFAULT 5, e BIGINT UNSIGNED ZEROFILL, f INT(5) ZEROFILL, g TINYINT(1), h DECIMAL(6,2) DEFAULT 1.50, i DECIMAL(10), j FLOAT, k DOUBLE, l FLOAT(7,3), m DOUBLE(8,2) UNSIGNED, n CHAR(3) NOT NULL DEFAULT 'x', o VARCHAR(10) COLLATE utf8mb4_bin, p VARCHAR(20) CHARACTER SET utf8mb3, q BINARY(4), r VARBINARY(8), s TINYTEXT, u TEXT, v MEDIUMTEXT, w LONGTEXT, x TINYBLOB, y BLOB, z MEDIUMBLOB, aa LONGBLOB, ab VARCHAR(5) DEFAULT '', ac INT DEFAULT NULL, PRIMARY KEY (a), UNIQUE KEY (b), KEY (c, d))
CREATE TABLE u (k1 INT, k2 INT, v INT UNIQUE, PRIMARY KEY (k1, k2), KEY (k2))
SELECT TABLE_CATALOG, TABLE_SCHEMA, TABLE_NAME, COLUMN_NAME, ORDINAL_POSITION, COLUMN_DEFAULT, IS_NULLABLE, DATA_TYPE, CHARACTER_MAXIMUM_LENGTH, CHARACTER_OCTET_LENGTH, NUMERIC_PRECISION, NUMERIC_SCALE, DATETIME_PRECISION, CHARACTER_SET_NAME, COLLATION_NAME, COLUMN_TYPE, COLUMN_KEY, EXTRA, PRIVILEGES, COLUMN_COMMENT, GENERATION_EXPRESSION, SRS_ID FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'kv_isc' ORDER BY TABLE_NAME, ORDINAL_POSITION
SELECT TABLE_CATALOG, TABLE_SCHEMA, TABLE_NAME, TABLE_TYPE, ENGINE, VERSION, ROW_FORMAT, TABLE_COLLATION, CHECKSUM, CREATE_OPTIONS, TABLE_COMMENT FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'kv_isc' ORDER BY TABLE_NAME
SELECT * FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = 'kv_isc'
SELECT schema_name FROM INFORMATION_SCHEMA.schemata WHERE schema_name LIKE 'kv_is%'
SELECT TABLE_SCHEMA AS TABLE_CAT, NULL AS TABLE_SCHEM, TABLE_NAME, CASE WHEN TABLE_TYPE='BASE TABLE' THEN CASE WHEN TABLE_SCHEMA = 'mysql' OR TABLE_SCHEMA = 'performance_schema' THEN 'SYSTEM TABLE' ELSE 'TABLE' END WHEN TABLE_TYPE='TEMPORARY' THEN 'LOCAL_TEMPORARY' ELSE TABLE_TYPE END AS TABLE_TYPE, TABLE_COMMENT AS REMARKS, NULL AS TYPE_CAT, NULL AS TYPE_SCHEM, NULL AS TYPE_NAME, NULL AS SELF_REFERENCING_COL_NAME, NULL AS REF_GENERATION FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = 'kv_isc' AND TABLE_NAME LIKE '%' HAVING TABLE_TYPE IN ('TABLE',null,null,null,null) ORDER BY TABLE_TYPE, TABLE_SCHEMA, TABLE_NAME
SELECT TABLE_SCHEMA, NULL, TABLE_NAME, COLUMN_NAME,CASE  WHEN UPPER(DATA_TYPE)='DECIMAL' THEN 3 WHEN UPPER(DATA_TYPE)='DECIMAL UNSIGNED' THEN 3 WHEN UPPER(DATA_TYPE)='TINYINT' THEN CASE WHEN LOCATE('ZEROFILL', UPPER(COLUMN_TYPE)) = 0 AND LOCATE('UNSIGNED', UPPER(COLUMN_TYPE)) = 0 AND LOCATE('(1)', COLUMN_TYPE) != 0 THEN -7 ELSE -6 END  WHEN UPPER(DATA_TYPE)='TINYINT UNSIGNED' THEN CASE WHEN LOCATE('ZEROFILL', UPPER(COLUMN_TYPE)) = 0 AND LOCATE('UNSIGNED', UPPER(COLUMN_TYPE)) = 0 AND LOCATE('(1)', COLUMN_TYPE) != 0 THEN -7 ELSE -6 END  WHEN UPPER(DATA_TYPE)='BOOLEAN' THEN 16 WHEN UPPER(DATA_TYPE)='SMALLINT' THEN 5 WHEN UPPER(DATA_TYPE)='SMALLINT UNSIGNED' THEN 5 WHEN UPPER(DATA_TYPE)='INT' THEN 4 WHEN UPPER(DATA_TYPE)='INT UNSIGNED' THEN 4 WHEN UPPER(DATA_TYPE)='FLOAT' THEN 7 WHEN UPPER(DATA_TYPE)='FLOAT UNSIGNED' THEN 7 WHEN UPPER(DATA_TYPE)='DOUBLE' THEN 8 WHEN UPPER(DATA_TYPE)='DOUBLE UNSIGNED' THEN 8 WHEN UPPER(DATA_TYPE)='NULL' THEN 0 WHEN UPPER(DATA_TYPE)='TIMESTAMP' THEN 93 WHEN UPPER(DATA_TYPE)='BIGINT' THEN -5 WHEN UPPER(DATA_TYPE)='BIGINT UNSIGNED' THEN -5 WHEN UPPER(DATA_TYPE)='MEDIUMINT' THEN 4 WHEN UPPER(DATA_TYPE)='MEDIUMINT UNSIGNED' THEN 4 WHEN UPPER(DATA_TYPE)='DATE' THEN 91 WHEN UPPER(DATA_TYPE)='TIME' THEN 92 WHEN UPPER(DATA_TYPE)='DATETIME' THEN 93 WHEN UPPER(DATA_TYPE)='YEAR' THEN 91 WHEN UPPER(DATA_TYPE)='VARCHAR' THEN 12 WHEN UPPER(DATA_TYPE)='VARBINARY' THEN -3 WHEN UPPER(DATA_TYPE)='BIT' THEN -7 WHEN UPPER(DATA_TYPE)='JSON' THEN -1 WHEN UPPER(DATA_TYPE)='ENUM' THEN 1 WHEN UPPER(DATA_TYPE)='SET' THEN 1 WHEN UPPER(DATA_TYPE)='TINYBLOB' THEN -3 WHEN UPPER(DATA_TYPE)='TINYTEXT' THEN 12 WHEN UPPER(DATA_TYPE)='MEDIUMBLOB' THEN -4 WHEN UPPER(DATA_TYPE)='MEDIUMTEXT' THEN -1 WHEN UPPER(DATA_TYPE)='LONGBLOB' THEN -4 WHEN UPPER(DATA_TYPE)='LONGTEXT' THEN -1 WHEN UPPER(DATA_TYPE)='BLOB' THEN -4 WHEN UPPER(DATA_TYPE)='TEXT' THEN -1 WHEN UPPER(DATA_TYPE)='CHAR' THEN 1 WHEN UPPER(DATA_TYPE)='BINARY' THEN -2 WHEN UPPER(DATA_TYPE)='GEOMETRY' THEN -2 WHEN UPPER(DATA_TYPE)='VECTOR' THEN -4 WHEN UPPER(DATA_TYPE)='UNKNOWN' THEN 1111 WHEN UPPER(DATA_TYPE)='POINT' THEN -2 WHEN UPPER(DATA_TYPE)='LINESTRING' THEN -2 WHEN UPPER(DATA_TYPE)='POLYGON' THEN -2 WHEN UPPER(DATA_TYPE)='MULTIPOINT' THEN -2 WHEN UPPER(DATA_TYPE)='MULTILINESTRING' THEN -2 WHEN UPPER(DATA_TYPE)='MULTIPOLYGON' THEN -2 WHEN UPPER(DATA_TYPE)='GEOMETRYCOLLECTION' THEN -2 WHEN UPPER(DATA_TYPE)='GEOMCOLLECTION' THEN -2 ELSE 1111 END  AS DATA_TYPE, UPPER(CASE WHEN UPPER(DATA_TYPE)='TINYINT' THEN CASE WHEN LOCATE('ZEROFILL', UPPER(COLUMN_TYPE)) = 0 AND LOCATE('UNSIGNED', UPPER(COLUMN_TYPE)) = 0 AND LOCATE('(1)', COLUMN_TYPE) != 0 THEN 'BIT' WHEN LOCATE('UNSIGNED', UPPER(COLUMN_TYPE)) != 0 AND LOCATE('UNSIGNED', UPPER(DATA_TYPE)) = 0 THEN 'TINYINT UNSIGNED' ELSE DATA_TYPE END  WHEN LOCATE('UNSIGNED', UPPER(COLUMN_TYPE)) != 0 AND LOCATE('UNSIGNED', UPPER(DATA_TYPE)) = 0 AND LOCATE('SET', UPPER(DATA_TYPE)) <> 1 AND LOCATE('ENUM', UPPER(DATA_TYPE)) <> 1 THEN CONCAT(DATA_TYPE, ' UNSIGNED') WHEN UPPER(DATA_TYPE)='POINT' THEN 'GEOMETRY' WHEN UPPER(DATA_TYPE)='LINESTRING' THEN 'GEOMETRY' WHEN UPPER(DATA_TYPE)='POLYGON' THEN 'GEOMETRY' WHEN UPPER(DATA_TYPE)='MULTIPOINT' THEN 'GEOMETRY' WHEN UPPER(DATA_TYPE)='MULTILINESTRING' THEN 'GEOMETRY' WHEN UPPER(DATA_TYPE)='MULTIPOLYGON' THEN 'GEOMETRY' WHEN UPPER(DATA_TYPE)='GEOMETRYCOLLECTION' THEN 'GEOMETRY' WHEN UPPER(DATA_TYPE)='GEOMCOLLECTION' THEN 'GEOMETRY' ELSE UPPER(DATA_TYPE) END) AS TYPE_NAME,UPPER(CASE WHEN UPPER(DATA_TYPE)='DATE' THEN 10 WHEN UPPER(DATA_TYPE)='TIME'  THEN 8+(CASE WHEN DATETIME_PRECISION>0 THEN DATETIME_PRECISION+1 ELSE DATETIME_PRECISION END) WHEN UPPER(DATA_TYPE)='DATETIME' OR  UPPER(DATA_TYPE)='TIMESTAMP'  THEN 19+(CASE WHEN DATETIME_PRECISION>0 THEN DATETIME_PRECISION+1 ELSE DATETIME_PRECISION END) WHEN UPPER(DATA_TYPE)='YEAR' THEN 4 WHEN UPPER(DATA_TYPE)='TINYINT' AND LOCATE('ZEROFILL', UPPER(COLUMN_TYPE)) = 0 AND LOCATE('UNSIGNED', UPPER(COLUMN_TYPE)) = 0 AND LOCATE('(1)', COLUMN_TYPE) != 0 THEN 1 WHEN UPPER(DATA_TYPE)='MEDIUMINT' AND LOCATE('UNSIGNED', UPPER(COLUMN_TYPE)) != 0 THEN 8 WHEN UPPER(DATA_TYPE)='JSON' THEN 1073741824 WHEN UPPER(DATA_TYPE)='GEOMETRY' THEN 65535 WHEN UPPER(DATA_TYPE)='POINT' THEN 65535 WHEN UPPER(DATA_TYPE)='LINESTRING' THEN 65535 WHEN UPPER(DATA_TYPE)='POLYGON' THEN 65535 WHEN UPPER(DATA_TYPE)='MULTIPOINT' THEN 65535 WHEN UPPER(DATA_TYPE)='MULTILINESTRING' THEN 65535 WHEN UPPER(DATA_TYPE)='MULTIPOLYGON' THEN 65535 WHEN UPPER(DATA_TYPE)='GEOMETRYCOLLECTION' THEN 65535 WHEN UPPER(DATA_TYPE)='GEOMCOLLECTION' THEN 65535 WHEN CHARACTER_MAXIMUM_LENGTH IS NULL THEN NUMERIC_PRECISION WHEN CHARACTER_MAXIMUM_LENGTH > 2147483647 THEN 2147483647 ELSE CHARACTER_MAXIMUM_LENGTH END) AS COLUMN_SIZE,65535 AS BUFFER_LENGTH,UPPER(CASE WHEN UPPER(DATA_TYPE)='DECIMAL' THEN NUMERIC_SCALE WHEN UPPER(DATA_TYPE)='FLOAT' OR UPPER(DATA_TYPE)='DOUBLE' THEN CASE WHEN NUMERIC_SCALE IS NULL THEN 0 ELSE NUMERIC_SCALE END ELSE NULL END) AS DECIMAL_DIGITS,10 AS NUM_PREC_RADIX,CASE WHEN IS_NULLABLE='NO' THEN 0 ELSE CASE WHEN IS_NULLABLE='YES' THEN 1 ELSE 2 END END AS NULLABLE,COLUMN_COMMENT AS REMARKS,COLUMN_DEFAULT AS COLUMN_DEF,0 AS SQL_DATA_TYPE,0 AS SQL_DATETIME_SUB,CASE WHEN CHARACTER_OCTET_LENGTH > 2147483647 THEN 2147483647 ELSE CHARACTER_OCTET_LENGTH END AS CHAR_OCTET_LENGTH,ORDINAL_POSITION, IS_NULLABLE, NULL AS SCOPE_CATALOG, NULL AS SCOPE_SCHEMA, NULL AS SCOPE_TABLE, NULL AS SOURCE_DATA_TYPE,IF (EXTRA LIKE '%auto_increment%','YES','NO') AS IS_AUTOINCREMENT, IF (EXTRA LIKE '%GENERATED%','YES','NO') AS IS_GENERATEDCOLUMN FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = 'kv_isc' AND TABLE_NAME = 't' AND COLUMN_NAME LIKE '%' ORDER BY TABLE_SCHEMA, TABLE_NAME, ORDINAL_POSITION
SELECT TABLE_SCHEMA AS TABLE_CAT, NULL AS TABLE_SCHEM, TABLE_NAME, CASE WHEN TABLE_TYPE='BASE TABLE' THEN CASE WHEN TABLE_SCHEMA = 'mysql' OR TABLE_SCHEMA = 'performance_schema' THEN 'SYSTEM TABLE' ELSE 'TABLE' END WHEN TABLE_TYPE='TEMPORARY' THEN 'LOCAL_TEMPORARY' ELSE TABLE_TYPE END AS TABLE_TYPE, TABLE_COMMENT AS REMARKS, NULL AS TYPE_CAT, NULL AS TYPE_SCHEM, NULL AS TYPE_NAME, NULL AS SELF_REFERENCING_COL_NAME, NULL AS REF_GENERATION FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = 'kv_isc' AND TABLE_NAME LIKE '%' ORDER BY TABLE_TYPE, TABLE_SCHEMA, TABLE_NAME
SELECT * FROM information_schema.NOPE
INSERT INTO information_schema.TABLES (TABLE_NAME) VALUES ('x')
UPDATE information_schema.SCHEMATA SET SCHEMA_NAME = 'x'
DELETE FROM information_schema.COLUMNS
DROP TABLE information_schema.TABLES
INSERT INTO information_schema.NOPE VALUES (1)
USE information_schema
SELECT DATABASE()
USE kv_isc
DROP DATABASE kv_isc
CREATE DATABASE kv_keys
USE kv_keys
CREATE TABLE p (a VARCHAR(10), UNIQUE KEY (a(3)))
INSERT INTO p VALUES ('abcdef')
INSERT INTO p VALUES ('abcxyz')
INSERT INTO p VALUES ('ab')
SELECT a FROM p ORDER BY a
CREATE TABLE pm (a VARCHAR(10), UNIQUE KEY (a(2)))
INSERT INTO pm VALUES ('éxa')
INSERT INTO pm VALUES ('éxb')
CREATE TABLE pb (a VARBINARY(10), UNIQUE KEY (a(2)))
INSERT INTO pb VALUES ('abc')
INSERT INTO pb VALUES ('abd')
CREATE TABLE n (a INT, b INT, UNIQUE KEY (a, b))
INSERT INTO n VALUES (1, NULL), (1, NULL)
SELECT COUNT(*) FROM n
CREATE TABLE u (a BIGINT UNSIGNED PRIMARY KEY)
INSERT INTO u VALUES (18446744073709551615), (1), (9223372036854775808)
SELECT a FROM u
INSERT INTO u VALUES (1)
CREATE TABLE d (a DECIMAL(6,2) PRIMARY KEY)
INSERT INTO d VALUES (1.5), (-2.25), (0), (-0.5), (10)
SELECT a FROM d
INSERT INTO d VALUES (1.50)
CREATE TABLE f (a DOUBLE PRIMARY KEY)
INSERT INTO f VALUES (1.5e0), (-2e0), (0e0), (-0.5e0)
SELECT a FROM f
INSERT INTO f VALUES (-0e0)
CREATE TABLE s (a SMALLINT, g SMALLINT UNSIGNED, b DECIMAL(40,30), c DOUBLE, d FLOAT(5,2), e VARCHAR(40))
INSERT INTO s (a) VALUES (40000)
INSERT INTO s (a) VALUES (-32768)
INSERT INTO s (g) VALUES (65536)
INSERT INTO s (g) VALUES (65535)
INSERT INTO s (b) VALUES ('0.000')
INSERT INTO s (b) VALUES ('1e-40')
SHOW WARNINGS
INSERT INTO s (b) VALUES ('0.001')
INSERT INTO s (b) VALUES ('0.1234567890123456789012345678901234567')
SHOW WARNINGS
INSERT INTO s (c) VALUES (18446744073709551615)
INSERT INTO s (c) VALUES (0x41)
INSERT INTO s (c) VALUES (0x010203040506070809)
INSERT INTO s (d) VALUES (123.456)
INSERT INTO s (d) VALUES (1000)
INSERT INTO s (e) VALUES (18446744073709551615), (1.5), (1.5e0)
INSERT INTO s (e) VALUES (0xC3)
INSERT INTO s (e) VALUES (0xF880808080)
INSERT INTO s (e) VALUES (0xE282)
SELECT * FROM s
CREATE TABLE z (a TINYINT ZEROFILL, b SMALLINT ZEROFILL, c MEDIUMINT ZEROFILL, d INT ZEROFILL, e BIGINT ZEROFILL)
SELECT COLUMN_NAME, COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'kv_keys' AND TABLE_NAME = 'z' ORDER BY ORDINAL_POSITION
CREATE TABLE t1 (a TEXT(70000), b BLOB(70000), c TEXT(17000000), d BLOB(17000000))
CREATE TABLE t2 (a CHAR(3) CHARACTER SET binary, b VARCHAR(3) CHARACTER SET binary, c TINYTEXT CHARACTER SET binary, d TEXT CHARACTER SET binary, e MEDIUMTEXT CHARACTER SET binary, f LONGTEXT CHARACTER SET binary)
SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'kv_keys' AND TABLE_NAME IN ('t1', 't2') ORDER BY TABLE_NAME, ORDINAL_POSITION
CREATE TABLE t3 (a VARCHAR(5) CHARACTER SET utf8mb4 COLLATE latin1_swedish_ci)
SELECT '﨎' < '㐀', '﨎' < '一', '﨩' < '㐀', '一' < '㐀'
SELECT SUM(0x41), SUM(b'101'), AVG(0x0102)
SELECT BIT_OR(18446744073709551615), BIT_OR(2.6), BIT_XOR('3'), BIT_AND(2.5e0)
SELECT BIT_OR(2.5), BIT_OR(3.5), BIT_OR(-2.5), BIT_OR(2.5e0), BIT_OR(3.5e0), BIT_OR('2.5'), BIT_OR('3.5'), BIT_OR(-1), BIT_OR(0x41)
SELECT VARIANCE(DISTINCT 1)
SELECT STD(DISTINCT 1)
SELECT BIT_AND(DISTINCT 1)
SET SESSION div_precision_increment = -1
SHOW WARNINGS
SELECT @@div_precision_increment
SET SESSION div_precision_increment = DEFAULT
SET SESSION time_zone = '+05:00'
SELECT @@time_zone
SET SESSION time_zone = 'system'
SELECT @@time_zone
SET SESSION time_zone = 'bogus'
SET SESSION time_zone = 5
SET SESSION sql_mode = 1.5
SET SESSION time_zone = DEFAULT
SELECT LEFT('abcdef', 0x02), REPEAT('a', b'11'), LPAD('a', 0x03, 'x')
SELECT LOCATE(1, 'a1b'), LOCATE('1', 213), LOCATE(2.5, 'x2.5')
SELECT ROUND(195.0, -1), ROUND(95.0, -1), ROUND(150.0, -2), ROUND(-95.0, -1)
SELECT ROUND(9223372036854775815, -1), TRUNCATE(9223372036854775815, -1), ROUND(9223372036854775814, -1)
SELECT LENGTH(LPAD('a', 40000000, 'é'))
SHOW WARNINGS
CREATE TABLE k1 (a INT, KEY (a) COMMENT 'x' USING BTREE)
CREATE TABLE k2 (a INT, KEY (a) USING HASH)
CREATE TABLE k3 (a INT, KEY (a) BOGUS)
CREATE TABLE k4 (a INT DEFAULT CURRENT_TIMESTAMP())
CREATE TABLE k5 (a INT FOO)
CREATE TABLE k6 (a INT DEFAULT +)
SHOW TABLES LIKE 'k%'
CREATE TABLE g (id INT PRIMARY KEY, a INT)
INSERT INTO g VALUES (1, 10), (2, 10), (3, 20)
SELECT * FROM g GROUP BY id
SELECT g.* FROM g GROUP BY id ORDER BY id DESC
SELECT kv_keys.g.* FROM g GROUP BY id
SELECT x.* FROM g GROUP BY id
SELECT a, COUNT(*) FROM g GROUP BY a ORDER BY 2, 1
SELECT a, COUNT(*) FROM g GROUP BY a ORDER BY 5
SELECT DISTINCT COUNT(*) FROM g GROUP BY id
SELECT id FROM g WHERE 18446744073709551615
SELECT id FROM g WHERE 0.5
SELECT id FROM g WHERE 0.0e0
SELECT id FROM g WHERE '1abc'
SHOW WARNINGS
SELECT id FROM g WHERE 'x'
SHOW WARNINGS
SELECT 9223372036854775807 + TRUE
SELECT 9223372036854775807 + (NOT 0)
SELECT ~0 + 1
SELECT 9223372036854775807 + (NULL IS NULL)
SELECT 18446744073709551615 + (1 = 1)
SELECT 9223372036854775807 + LENGTH('ab')
SELECT 9223372036854775807 + (2 > 1 AND 3 > 2)
SELECT 2.5e0 | 0, 3.5e0 | 0, 2.5 | 0, '3' | 0, '3.5' | 0, 0x41 | 0, 8 >> 2, 1 >> 64
SELECT 18446744073709551615 DIV 1, 18446744073709551615 DIV -1
SELECT 5 DIV 2.0, 18446744073709551615 DIV 2
DROP DATABASE kv_keys
CREATE DATABASE kv_parse
USE kv_parse
CREATE TABLE a1 (c CHAR(1) CHARACTER SET 1)
CREATE TABLE a4 (c LONG VARBINARY, d LONG VARCHAR, e LONG, f LONG CHARACTER SET utf8mb3)
CREATE TABLE a5 (c INT, d CHAR(1)) CHARACTER SET = utf8mb3
CREATE TABLE a6 (c INT, d CHAR(1)) DEFAULT CHARACTER SET utf8mb3 COLLATE utf8mb3_bin
CREATE TABLE a7 (c INT) 5
SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE, CHARACTER_SET_NAME, COLLATION_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = 'kv_parse' ORDER BY TABLE_NAME, ORDINAL_POSITION
SELECT TABLE_NAME, TABLE_COLLATION FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'kv_parse' ORDER BY TABLE_NAME
START TRANSACTION READ
START TRANSACTION 5
SELECT 1 LIMIT 'a'
SET SESSION div_precision_increment = 18446744073709551615
SHOW WARNINGS
SELECT @@div_precision_increment
SET SESSION div_precision_increment = DEFAULT
SET SESSION sql_select_limit = 18446744073709551615
SELECT @@sql_select_limit
CREATE TABLE g (id INT PRIMARY KEY, a INT)
INSERT INTO g VALUES (1, 10), (2, 10), (3, 20)
SELECT a, COUNT(*) FROM g WHERE '1abc' GROUP BY a
SHOW WARNINGS
SELECT a, COUNT(*) FROM g WHERE a > '1x' GROUP BY a
SHOW WARNINGS
SELECT a, COUNT(*) FROM g GROUP BY a HAVING 'x'
SHOW WARNINGS
SELECT a, COUNT(*) FROM g GROUP BY a HAVING COUNT(*) > '1x'
SHOW WARNINGS
SELECT a, COUNT(*) FROM g GROUP BY a HAVING 0.5e0
SELECT a FROM g GROUP BY a HAVING 18446744073709551615
SELECT a FROM g GROUP BY a HAVING 0.4
SELECT a, COUNT(*), SUM(a), AVG(a) FROM g WHERE 0 GROUP BY a
SELECT COUNT(*) FROM g HAVING 0
SELECT id FROM g WHERE a > '1x'
SHOW WARNINGS
SELECT id, a + '1x' FROM g
SHOW WARNINGS
UPDATE g SET a = a WHERE a > '1x'
SHOW WARNINGS
SELECT 1 WHERE '1abc'
SHOW WARNINGS
SELECT 1 WHERE 0.0
SELECT 1 FROM DUAL WHERE 18446744073709551615
SELECT 1 WHERE 0.5e0
UPDATE g SET a = a + 1 WHERE '1abc'
SHOW WARNINGS
DELETE FROM g WHERE 'x'
SHOW WARNINGS
UPDATE g SET a = 0 WHERE 0.0e0
UPDATE g SET a = a + 1 WHERE 0.5
DELETE FROM g WHERE 18446744073709551615 AND id = 3
SELECT * FROM g
SET SESSION sql_mode = ''
UPDATE g SET a = a + 1 WHERE '1abc'
SHOW WARNINGS
DELETE FROM g WHERE 'x'
SHOW WARNINGS
SET SESSION sql_mode = DEFAULT
SELECT * FROM g
CREATE TABLE n (s SMALLINT, m MEDIUMINT, us SMALLINT UNSIGNED)
INSERT INTO n VALUES (-3, 70000, 5)
SELECT s + 1, m + 1, s * m, -s, -m, s DIV 2, us + 1, s | 0 FROM n
SELECT MIN(s), MAX(m), MIN(us) FROM n
SELECT CASE WHEN 1 THEN s END, CASE WHEN 1 THEN m END, CASE WHEN 1 THEN us END FROM n
SELECT '1.5e3abc' + 0, '2e' + 0, '2e+' + 0, '2E-1' + 0, ' 3e2 ' + 0
SHOW WARNINGS
SELECT 9223372036854775807 + (NOT (1 = 1))
SELECT 9223372036854775807 + (NOT (1 = 1)) + 1
SELECT 9223372036854775807 + CASE WHEN 1 THEN 1 END
SELECT 9223372036854775807 + (1 IN (1, 2))
SELECT 9223372036854775807 + (2 BETWEEN 1 AND 3)
SELECT 9223372036854775807 + ('a' LIKE 'a')
SELECT 9223372036854775807 + (NOT (1 < 2)) + 1
SELECT 9223372036854775807 + (NOT (1 <=> 2)) + 1
SELECT 9223372036854775807 + (NOT (1 AND 2)) + 1
SELECT 9223372036854775807 + (NOT (1 > 2 OR 0)) + 1
SELECT 9223372036854775807 + CASE 1 WHEN 1 THEN 1 WHEN 2 THEN 2 ELSE 3 END
SELECT 9223372036854775807 + (1 NOT IN (1, 2)) + 1
SELECT 9223372036854775807 + (2 NOT BETWEEN 1 AND 3) + 1
SELECT 9223372036854775807 + ('a' NOT LIKE 'b')
SELECT 9223372036854775807 + ('a' LIKE 'a' ESCAPE '|')
SELECT 9223372036854775807 + (NOT (1 IN (1, 2))) + 1
SELECT 9223372036854775807 + (NOT (2 BETWEEN 1 AND 3)) + 1
SELECT 9223372036854775807 + (NOT ('a' LIKE 'b'))
DELETE FROM g
INSERT INTO g VALUES (1, 10), (2, 10), (3, 20)
SELECT * FROM g ORDER BY 1 DESC
SELECT *, id FROM g ORDER BY 3 DESC
SELECT id FROM g ORDER BY 2
SELECT 9223372036854775807 + id FROM g
SELECT a, COUNT(*) FROM g WHERE id + 9223372036854775807 > 0 GROUP BY a
SELECT a + 1 FROM g GROUP BY a + 2
SELECT a + 1 FROM g GROUP BY a + 1
UPDATE g SET a = a ORDER BY a, id DESC LIMIT 2
UPDATE g SET a = a ORDER BY a LIMIT 3
CREATE TABLE d (id INT PRIMARY KEY, x INT NOT NULL, y INT)
INSERT INTO d VALUES (1, 5, 6)
UPDATE d SET y = DEFAULT
UPDATE d SET x = DEFAULT
SELECT * FROM d
CREATE TABLE u (c VARCHAR(5))
INSERT INTO u VALUES (0xFF41)
INSERT INTO u VALUES (0x41FF42434445464748)
CREATE TABLE information_schema.t (a INT)
SET NAMES utf8mb4 COLLATE latin1_swedish_ci
SET NAMES utf8mb4 COLLATE utf8mb3_bin
SET NAMES utf8mb4 COLLATE nope_ci
CREATE DATABASE kv_c CHARACTER SET utf8mb4 COLLATE utf8mb3_bin
CREATE DATABASE kv_c CHARACTER SET utf8mb3 COLLATE utf8mb4_bin
CREATE DATABASE kv_c CHARACTER SET utf8 COLLATE utf8mb4_bin
CREATE DATABASE kv_c CHARACTER SET utf8mb4 COLLATE binary
INSERT INTO u VALUES (0x41FF4243444546)
SELECT 9223372036854775807 + x.id FROM g x
SELECT 9223372036854775807 + kv_parse.g.id FROM g
SELECT a, COUNT(*) FROM g GROUP BY a HAVING a + 9223372036854775807 > 0
SELECT a AS x, COUNT(*) FROM g GROUP BY a HAVING x + 9223372036854775807 > 0
SELECT a + 1 AS x, COUNT(*) FROM g GROUP BY a HAVING x + 9223372036854775807 > 0
SELECT COUNT(*) AS x FROM g GROUP BY a HAVING x + 9223372036854775807 > 0
SET collation_connection = utf8mb4_bin
SET character_set_connection = utf8mb4
SELECT @@collation_connection
SET NAMES DEFAULT
DROP DATABASE kv_parse
CREATE DATABASE kv_union
CREATE TABLE kv_union.t (id INT PRIMARY KEY, ti TINYINT, si SMALLINT, i INT NOT NULL, bi BIGINT, ub BIGINT UNSIGNED, dc DECIMAL(10,3), dbl DOUBLE, fl FLOAT, vc VARCHAR(10), ch CHAR(5), tx TEXT, vb VARBINARY(8), d DATE, t TIME(2), dt DATETIME(3), ts TIMESTAMP NULL, y YEAR)
INSERT INTO kv_union.t VALUES (1, 1, 2, 3, 4, 5, 6.5, 7.5e0, 8.5, 'nine', 'ten', 'eleven', X'0c', '2024-02-29', NULL, '2024-02-29 12:34:56.789', '2024-02-29 12:34:56', 2024), (2, -1, NULL, 0, NULL, 18446744073709551615, -0.125, NULL, NULL, 'Nine', 'ten ', NULL, NULL, NULL, NULL, '2024-02-29 00:00:00', NULL, NULL)
SELECT ti AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT ti FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT si FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT i FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT bi FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT ub FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT dc FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT dbl FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT fl FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT vc FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT ch FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT tx FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT vb FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT d FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT t FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT dt FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT ts FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT y FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT ti AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT si AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT i AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT bi AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT ub AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT dc AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT dbl AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT fl AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT vc AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT ch AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT tx AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT vb AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT d AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT t AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT dt AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT ts AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT y AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT 1 AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT 1 AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT 1 AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT 1 AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT 1 AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT 2.5 AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT 2.5 AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT 2.5 AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT 2.5 AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT 2.5 AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT 'xy' AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT 'xy' AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT 'xy' AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT 'xy' AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT 'xy' AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT NULL AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT NULL AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT NULL AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT NULL AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT NULL AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT 1e0 AS x FROM kv_union.t UNION SELECT 1 FROM kv_union.t
SELECT 1e0 AS x FROM kv_union.t UNION SELECT 2.5 FROM kv_union.t
SELECT 1e0 AS x FROM kv_union.t UNION SELECT 'xy' FROM kv_union.t
SELECT 1e0 AS x FROM kv_union.t UNION SELECT NULL FROM kv_union.t
SELECT 1e0 AS x FROM kv_union.t UNION SELECT 1e0 FROM kv_union.t
SELECT 1 AS a UNION SELECT 2 UNION SELECT 1
SELECT 1 AS a UNION ALL SELECT 2 UNION ALL SELECT 1
SELECT 1 a, 'x' b UNION SELECT 2.5, 'yy'
SELECT NULL UNION SELECT NULL
SELECT DATE '2024-01-01' UNION SELECT TIMESTAMP '2024-01-01 10:00:00'
SELECT TIME '10:00:00' a UNION SELECT TIME '-01:00:00.5' UNION SELECT TIME '100:00:00'
SELECT 1 INTERSECT SELECT 1
SELECT 1 EXCEPT SELECT 2
(SELECT 3 a) UNION (SELECT 1) ORDER BY a DESC LIMIT 2
SELECT 1 UNION SELECT 1, 2
SELECT 3 a UNION SELECT 1 ORDER BY 1
SELECT 3 a UNION SELECT 1 ORDER BY b
SELECT 3 a UNION SELECT 1 ORDER BY 2
SELECT 1 a UNION SELECT 2 INTERSECT SELECT 3
SELECT 1 a UNION ALL SELECT 1 UNION SELECT 1
SELECT 1 a UNION SELECT 1 UNION ALL SELECT 1
SELECT 1 a UNION ALL SELECT 1 INTERSECT ALL SELECT 1
SELECT 1 a UNION ALL SELECT 1 EXCEPT ALL SELECT 1
SELECT 1 a UNION ALL SELECT 1 EXCEPT SELECT 2
(SELECT 3 a ORDER BY a LIMIT 1) UNION ALL (SELECT 1 UNION SELECT 2 ORDER BY 1 DESC LIMIT 1)
SELECT 3 a LIMIT 1 UNION SELECT 1
SELECT 3 a ORDER BY a UNION SELECT 1
SELECT 3 a UNION SELECT 1 ORDER BY a + 1
SELECT 3 a UNION SELECT 1 LIMIT 1, 1
SELECT 3 a UNION SELECT 1 LIMIT 1 OFFSET 1
((SELECT 1 a)) UNION ((SELECT 2))
(SELECT 2 a) ORDER BY a
(SELECT 2 a UNION SELECT 1) ORDER BY a LIMIT 1
SELECT 1 a UNION DISTINCT SELECT 1
SELECT 'a' a UNION SELECT 'A'
SELECT 'a ' a UNION SELECT 'a'
SELECT 'a' COLLATE utf8mb4_bin a UNION SELECT 'A' COLLATE utf8mb4_0900_ai_ci
SELECT 'a' COLLATE utf8mb4_bin a UNION SELECT 'A'
SELECT 1 a FROM DUAL WHERE FALSE UNION SELECT 2
SELECT 1 AS x UNION SELECT 2.5 UNION SELECT 1e0
SELECT 1 AS x UNION SELECT 2.25
SELECT 0.1e0 + 0.2e0 AS x UNION SELECT 'x'
SELECT DATE '2024-01-01' AS x UNION SELECT CAST('2024-01-01 10:00:00.5' AS DATETIME(1))
SELECT CAST('a' AS CHAR(3)) AS x UNION SELECT 'b'
SELECT 1 + 1 AS x UNION SELECT 2
SELECT CONCAT('a', 'b') AS x UNION SELECT 1
SELECT 18446744073709551615 AS x UNION SELECT -1
SELECT 1 x UNION SELECT 2 INTERSECT SELECT 2 EXCEPT SELECT 1
SELECT 1 x UNION ALL SELECT 1 UNION ALL SELECT 2 INTERSECT ALL SELECT 2 INTERSECT ALL SELECT 2
SELECT vc FROM kv_union.t UNION SELECT ch FROM kv_union.t ORDER BY vc DESC
SELECT vc, ch FROM kv_union.t UNION ALL SELECT ch, vc FROM kv_union.t ORDER BY 2, 1
SELECT id, vc FROM kv_union.t INTERSECT SELECT id, vc FROM kv_union.t WHERE id = 2
SELECT id FROM kv_union.t EXCEPT SELECT 2
SELECT i FROM kv_union.t UNION SELECT i + 1 FROM kv_union.t ORDER BY i DESC LIMIT 2
SELECT COUNT(*) FROM kv_union.t UNION SELECT SUM(i) FROM kv_union.t
SELECT 1 UNION SELECT * FROM kv_union.t
CREATE TABLE kv_union.u (a INT PRIMARY KEY)
INSERT INTO kv_union.u SELECT 1 UNION SELECT 2
INSERT INTO kv_union.u (SELECT 3 UNION SELECT 4 ORDER BY 1 DESC LIMIT 1)
SELECT a FROM kv_union.u ORDER BY a
DROP DATABASE kv_union
CREATE DATABASE kv_derived
CREATE TABLE kv_derived.t (id INT PRIMARY KEY, ti TINYINT, si SMALLINT, i INT NOT NULL, bi BIGINT, ub BIGINT UNSIGNED, dc DECIMAL(10,3), dbl DOUBLE, fl FLOAT, vc VARCHAR(10), ch CHAR(5), tx TEXT, vb VARBINARY(8), d DATE, t TIME(2), dt DATETIME(3), ts TIMESTAMP NULL, y YEAR)
SELECT x FROM (SELECT 1 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT -1 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT 123456789 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT 1234567890 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT 12345678901 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT 2.5 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT 1e0 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT 'xy' AS x FROM kv_derived.t) q
SELECT x FROM (SELECT NULL AS x FROM kv_derived.t) q
SELECT x FROM (SELECT 1+1 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT ti AS x FROM kv_derived.t) q
SELECT x FROM (SELECT si AS x FROM kv_derived.t) q
SELECT x FROM (SELECT i AS x FROM kv_derived.t) q
SELECT x FROM (SELECT bi AS x FROM kv_derived.t) q
SELECT x FROM (SELECT ub AS x FROM kv_derived.t) q
SELECT x FROM (SELECT dc AS x FROM kv_derived.t) q
SELECT x FROM (SELECT dbl AS x FROM kv_derived.t) q
SELECT x FROM (SELECT fl AS x FROM kv_derived.t) q
SELECT x FROM (SELECT vc AS x FROM kv_derived.t) q
SELECT x FROM (SELECT ch AS x FROM kv_derived.t) q
SELECT x FROM (SELECT tx AS x FROM kv_derived.t) q
SELECT x FROM (SELECT vb AS x FROM kv_derived.t) q
SELECT x FROM (SELECT d AS x FROM kv_derived.t) q
SELECT x FROM (SELECT t AS x FROM kv_derived.t) q
SELECT x FROM (SELECT dt AS x FROM kv_derived.t) q
SELECT x FROM (SELECT ts AS x FROM kv_derived.t) q
SELECT x FROM (SELECT y AS x FROM kv_derived.t) q
SELECT x FROM (SELECT i+1 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT dc*2 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT CONCAT(vc,'a') AS x FROM kv_derived.t) q
SELECT x FROM (SELECT COUNT(*) AS x FROM kv_derived.t) q
SELECT x FROM (SELECT SUM(i) AS x FROM kv_derived.t) q
SELECT x FROM (SELECT MAX(vc) AS x FROM kv_derived.t) q
SELECT x FROM (SELECT AVG(i) AS x FROM kv_derived.t) q
SELECT x FROM (SELECT DATE '2024-01-01' AS x FROM kv_derived.t) q
SELECT x FROM (SELECT NOW() AS x FROM kv_derived.t) q
SELECT x FROM (SELECT CAST(1 AS UNSIGNED) AS x FROM kv_derived.t) q
SELECT x FROM (SELECT CAST(1 AS DECIMAL(5,2)) AS x FROM kv_derived.t) q
SELECT x FROM (SELECT 18446744073709551615 AS x FROM kv_derived.t) q
SELECT x FROM (SELECT TRUE AS x FROM kv_derived.t) q
SELECT x FROM (SELECT X'41' AS x FROM kv_derived.t) q
SELECT x FROM (SELECT IF(1,1,2) AS x FROM kv_derived.t) q
SELECT x FROM (SELECT COALESCE(i,ti) AS x FROM kv_derived.t) q
CREATE TABLE kv_derived.s (id INT PRIMARY KEY, a INT, b VARCHAR(5))
INSERT INTO kv_derived.s VALUES (1, 10, 'x'), (2, 20, 'y'), (3, NULL, 'x')
SELECT x, y FROM (SELECT 1 x, 'a' y UNION SELECT 2, 'b') q ORDER BY x
SELECT * FROM (SELECT 1) q
SELECT x FROM (SELECT 1 x)
SELECT q.a FROM (SELECT a, b FROM kv_derived.s WHERE a > 5) AS q ORDER BY 1
SELECT b, COUNT(*), SUM(a) FROM (SELECT * FROM kv_derived.s) q GROUP BY b ORDER BY b
SELECT * FROM (SELECT id, a * 2 AS twice FROM kv_derived.s ORDER BY id DESC LIMIT 2) AS q ORDER BY id
SELECT twice FROM (SELECT id, a * 2 AS twice FROM kv_derived.s) q WHERE twice > 25
SELECT DISTINCT b FROM (SELECT b FROM kv_derived.s) q ORDER BY b
SELECT c FROM (SELECT COUNT(*) c FROM kv_derived.s) q
SELECT y FROM (SELECT DATE '2024-02-29' + INTERVAL id DAY y FROM kv_derived.s) q ORDER BY y
SELECT x FROM ((SELECT 3 x) UNION (SELECT 4)) q ORDER BY x DESC
SELECT z.n FROM (SELECT n FROM (SELECT 7 n) inner_q) z
SELECT nope FROM (SELECT 1 x) q
SELECT q.x FROM (SELECT 1 x) AS q WHERE q.x = 1
DROP DATABASE kv_derived
CREATE DATABASE kv_sub
CREATE TABLE kv_sub.t (id INT PRIMARY KEY, a INT, b VARCHAR(5))
INSERT INTO kv_sub.t VALUES (1, 10, 'x'), (2, 20, 'y'), (3, NULL, 'x')
CREATE TABLE kv_sub.u (id INT PRIMARY KEY, t_id INT, v DECIMAL(5,2))
INSERT INTO kv_sub.u VALUES (1, 1, 1.5), (2, 1, 2.5), (3, 2, 4.25)
SELECT (SELECT 1) a, (SELECT 'x') b, (SELECT NULL) c, (SELECT 1 FROM DUAL WHERE FALSE) d, (SELECT a FROM kv_sub.t WHERE id = 1) e, (SELECT MAX(a) FROM kv_sub.t) f
SELECT (SELECT a FROM kv_sub.t)
SELECT (SELECT 1, 2)
SELECT id, (SELECT b FROM kv_sub.t u WHERE u.id = t.id + 1) nxt FROM kv_sub.t t ORDER BY id
SELECT a, b FROM kv_sub.t WHERE a IN (SELECT a FROM kv_sub.t WHERE b = 'x') ORDER BY id
SELECT id FROM kv_sub.t WHERE a NOT IN (SELECT a FROM kv_sub.t WHERE b = 'x') ORDER BY id
SELECT 1 IN (SELECT a FROM kv_sub.t), 10 IN (SELECT a FROM kv_sub.t), NULL IN (SELECT a FROM kv_sub.t), 1 NOT IN (SELECT a FROM kv_sub.t), 1 IN (SELECT a FROM kv_sub.t WHERE FALSE)
SELECT EXISTS (SELECT 1), EXISTS (SELECT 1 FROM DUAL WHERE FALSE), NOT EXISTS (SELECT * FROM kv_sub.t)
SELECT 15 > ANY (SELECT a FROM kv_sub.t), 15 > ALL (SELECT a FROM kv_sub.t), 5 < ALL (SELECT a FROM kv_sub.t WHERE a IS NOT NULL), 15 = SOME (SELECT a FROM kv_sub.t), 1 > ALL (SELECT 1 FROM DUAL WHERE FALSE), 1 > ANY (SELECT 1 FROM DUAL WHERE FALSE)
SELECT (SELECT SUM(v) FROM kv_sub.u WHERE u.t_id = t.id) total FROM kv_sub.t t ORDER BY id
SELECT a, b FROM kv_sub.t t WHERE EXISTS (SELECT 1 FROM kv_sub.u WHERE u.t_id = t.id) ORDER BY id
SELECT a, b FROM kv_sub.t t WHERE NOT EXISTS (SELECT 1 FROM kv_sub.u WHERE u.t_id = t.id) ORDER BY id
SELECT id FROM kv_sub.t WHERE id = (SELECT MAX(t_id) FROM kv_sub.u)
SELECT id FROM kv_sub.t WHERE a > (SELECT AVG(a) FROM kv_sub.t)
SELECT (SELECT COUNT(*) FROM kv_sub.u WHERE t_id = t.id) + 1 AS n FROM kv_sub.t t ORDER BY id
SELECT b, (SELECT MIN(id) FROM kv_sub.t u WHERE u.b = t.b) FROM kv_sub.t t ORDER BY id
SELECT id FROM kv_sub.t WHERE id IN (SELECT t_id FROM kv_sub.u) AND b = 'x'
SELECT (SELECT (SELECT 7))
SELECT id FROM kv_sub.t t WHERE a = (SELECT a FROM kv_sub.t u WHERE u.id = t.id) ORDER BY id
SELECT (SELECT v FROM kv_sub.u WHERE id = 3) * 2, (SELECT b FROM kv_sub.t WHERE id = 2) = 'y'
SELECT x FROM (SELECT (SELECT 5) x) q
UPDATE kv_sub.t SET a = (SELECT MAX(id) FROM kv_sub.u) WHERE id = 3
SELECT a FROM kv_sub.t WHERE id = 3
DELETE FROM kv_sub.u WHERE t_id IN (SELECT id FROM kv_sub.t WHERE b = 'y')
SELECT COUNT(*) FROM kv_sub.u
DROP DATABASE kv_sub
CREATE DATABASE kv_dj
CREATE TABLE kv_dj.t (id INT PRIMARY KEY, a INT, b VARCHAR(5))
INSERT INTO kv_dj.t VALUES (1, 10, 'x'), (2, 20, 'y'), (3, NULL, 'x')
CREATE TABLE kv_dj.u (id INT PRIMARY KEY, t_id INT, v DECIMAL(5,2))
INSERT INTO kv_dj.u VALUES (1, 1, 1.5), (2, 1, 2.5), (3, 2, 4.25)
SELECT t.b, q.total FROM kv_dj.t t JOIN (SELECT t_id, SUM(v) total FROM kv_dj.u GROUP BY t_id) q ON q.t_id = t.id ORDER BY t.b, q.total
SELECT t.b, q.total FROM kv_dj.t t LEFT JOIN (SELECT t_id, SUM(v) total FROM kv_dj.u GROUP BY t_id) q ON q.t_id = t.id ORDER BY t.a
SELECT q.x, r.y FROM (SELECT 1 x UNION SELECT 2) q, (SELECT 'a' y UNION SELECT 'b') r ORDER BY q.x, r.y
SELECT q.n, u.v FROM (SELECT id n FROM kv_dj.t WHERE a > 5) q JOIN kv_dj.u u ON u.t_id = q.n ORDER BY u.v
SELECT COUNT(*) FROM kv_dj.t t JOIN (SELECT DISTINCT b FROM kv_dj.t) q USING (b)
SELECT q.b, COUNT(*) FROM (SELECT b FROM kv_dj.t) q JOIN kv_dj.u ON TRUE GROUP BY q.b ORDER BY q.b
SELECT t.a FROM kv_dj.t t JOIN (SELECT 1) AS q1 JOIN (SELECT 1) AS q1 ON TRUE
DROP DATABASE kv_dj
