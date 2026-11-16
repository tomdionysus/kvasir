#!/bin/sh
#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Records what the reference MySQL 8.4 server answers to a list of statements,
# as the differential gate's expected results (the design, "Proving it").
#
#   tools/record_mysql.sh statements.sql > expected.txt
#
# Each line of the input is one statement, without its semicolon, run in
# order in one session as a utf8mb4 client; blank lines and lines starting
# with "#" are skipped. Runs where `docker exec kvasir-mysql84` reaches the
# server (node-b), with the root password in its environment. The
# output, per statement:
#
#   statement <the statement>
#   column <name>|<type>|<collation id>|<length>|<decimals>|<flags>
#   row <value>\t<value>...        as the text protocol has them; \N for NULL
#
# The mysql client gives column types only in table mode, so values are read
# from its table: space at either end of a value, a "|" inside one, and the
# string 'NULL' cannot be told apart, and statements recorded avoid them.
#   error <code>|<sqlstate>|<message>
#   end
#
# Kvasir's test replays the statements through a session and compares.

set -eu

input=$1
container=${KVASIR_MYSQL_CONTAINER:-kvasir-mysql84}
statements=$(mktemp)
trap 'rm -f "$statements"' EXIT
grep -v -e '^[[:space:]]*$' -e '^#' "$input" > "$statements"

# Each statement between numbered markers the client prints itself (its
# "system" command), so no query of the recorder's touches the session's
# diagnostics between a statement and a SHOW WARNINGS after it.
awk '{ print "\\! echo kvasir_case_start " NR; print $0 ";"; print "\\! echo kvasir_case_end" }' "$statements" |
  docker exec -i "$container" sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" --default-character-set=utf8mb4 --column-type-info --force -t --raw -n 2>&1' |
  grep -v 'Using a password' |
  awk '
    NR == FNR { statement[FNR] = $0; next }
    /^Field +[0-9]+:/ { name = $0; sub(/^Field +[0-9]+: +`/, "", name); sub(/`$/, "", name); next }
    /^(Catalog|Database|Table|Org_table|Max_length):/ { next }
    /^Type:/ { type = $2; next }
    /^Collation:/ { id = $0; sub(/.*\(/, "", id); sub(/\).*/, "", id); next }
    /^Length:/ { length_ = $2; next }
    /^Decimals:/ { decimals = $2; next }
    /^Flags:/ {
      flags = $0; sub(/^Flags: */, "", flags); sub(/ *$/, "", flags)
      print "column " name "|" type "|" id "|" length_ "|" decimals "|" flags
      next
    }
    /^kvasir_case_start [0-9]+$/ { print "statement " statement[$2 + 0]; next }
    /^kvasir_case_end$/ { print "end"; next }
    /^$/ { next }
    # A table: a border, the header, a border, the rows, a border.
    /^\+/ { borders++; next }
    /^\|/ {
      if (borders % 3 == 1) next
      n = split(substr($0, 2, length($0) - 2), cell, "|")
      line = ""
      for (i = 1; i <= n; i++) {
        v = cell[i]; sub(/^ /, "", v); sub(/ $/, "", v); sub(/^ +/, "", v); sub(/ +$/, "", v)
        if (v == "NULL") v = "\\N"
        line = line (i > 1 ? "\t" : "") v
      }
      print "row " line
      next
    }
    /^ERROR [0-9]+ \(/ {
      code = $2; state = $3; gsub(/[()]/, "", state)
      message = $0; sub(/^ERROR [0-9]+ \([0-9A-Z]+\)( at line [0-9]+)?: /, "", message)
      print "error " code "|" state "|" message
      next
    }
  ' "$statements" -
