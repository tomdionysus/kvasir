//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>

// The general_ci collations' weight for each BMP code point, as
// kvasir-general-ci derives it at build time from
// kvasir/data/unicode/UnicodeData-3.0.0.txt (ruling 33).
namespace kvasir::exec::general_ci {

extern const uint16_t weights[65536];

}  // namespace kvasir::exec::general_ci
