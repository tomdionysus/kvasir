# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The differential gate's first list: statements a session runs with no
# table, one per line, in one session. tools/record_mysql.sh runs them on the
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
