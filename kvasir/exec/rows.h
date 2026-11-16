//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>

#include "kvasir/catalog/table.h"
#include "kvasir/exec/value.h"
#include "kvasir/txn/store.h"

// A table's rows as SQL reads them: a stored value back as a value, a value
// as a client is sent it (ZEROFILL's zeros), and the bytes a key's values
// sort as in the store, equal exactly when MySQL finds two keys equal (under
// the key's collation; PAD SPACE collations pad to the key's length, as
// MySQL's own key images do).
namespace kvasir::exec {

// A column's stored text as a value: an integer, a decimal, a double (a
// FLOAT as the float it holds), or the string.
Value value_of(const catalog::ColumnType& type, const std::string& text);

// The text a client is sent for a stored value: ZEROFILL pads with zeros to
// `display_length`.
std::string display_text(const catalog::ColumnType& type, const std::string& text, uint32_t display_length);

// Whether any of the key's columns is NULL in the row (a unique key does not
// hold such a row).
bool key_has_null(const catalog::Key& key, const txn::Row& row);

// The bytes the row's values in the key sort as.
std::string encode_key(const catalog::Table& table, const catalog::Key& key, const txn::Row& row);

// The key's values as error 1062 names them, joined by '-'.
std::string key_text(const catalog::Key& key, const txn::Row& row);

// Values in ORDER BY: NULL first, numbers by value, strings under
// `collation` (an id; 0 for none). -1, 0 or 1.
int compare_for_order(const Value& a, const Value& b, uint16_t collation);

}  // namespace kvasir::exec
