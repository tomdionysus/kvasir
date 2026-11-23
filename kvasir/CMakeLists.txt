#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The database. Public headers are kvasir/include/kvasir/*.h, included as
# "kvasir/x.h"; everything else is included from the repository root as
# "kvasir/<subsystem>/x.h".
# Unicode's collation table (ruling 32), compiled into C++ by kvasir-ducet.
set(kvasir_ducet_table ${PROJECT_BINARY_DIR}/generated/kvasir/ducet_table.cpp)
add_custom_command(
  OUTPUT ${kvasir_ducet_table}
  COMMAND kvasir-ducet ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode/allkeys-9.0.0.txt ${kvasir_ducet_table}
  DEPENDS kvasir-ducet ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode/allkeys-9.0.0.txt
  COMMENT "Compiling Unicode's collation table"
)

# The general_ci weights (ruling 33), derived by kvasir-general-ci.
set(kvasir_general_ci_table ${PROJECT_BINARY_DIR}/generated/kvasir/general_ci_table.cpp)
add_custom_command(
  OUTPUT ${kvasir_general_ci_table}
  COMMAND kvasir-general-ci ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode/UnicodeData-3.0.0.txt ${kvasir_general_ci_table}
  DEPENDS kvasir-general-ci ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode/UnicodeData-3.0.0.txt
  COMMENT "Deriving the general_ci weights from Unicode's character database"
)

# UPPER() and LOWER()'s case tables, compiled by kvasir-casing: Unicode
# 9.0.0's for utf8mb4's 0900 and _bin collations, 3.0.0's (the BMP) for the
# general_ci and utf8mb3 ones.
set(kvasir_casing9_table ${PROJECT_BINARY_DIR}/generated/kvasir/casing9_table.cpp)
add_custom_command(
  OUTPUT ${kvasir_casing9_table}
  COMMAND kvasir-casing ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode/UnicodeData-9.0.0.txt 110000 unicode9 ${kvasir_casing9_table}
  DEPENDS kvasir-casing ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode/UnicodeData-9.0.0.txt
  COMMENT "Compiling Unicode 9.0.0's case mappings"
)
set(kvasir_casing3_table ${PROJECT_BINARY_DIR}/generated/kvasir/casing3_table.cpp)
add_custom_command(
  OUTPUT ${kvasir_casing3_table}
  COMMAND kvasir-casing ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode/UnicodeData-3.0.0.txt 10000 unicode3 ${kvasir_casing3_table}
  DEPENDS kvasir-casing ${CMAKE_CURRENT_SOURCE_DIR}/data/unicode/UnicodeData-3.0.0.txt
  COMMENT "Compiling Unicode 3.0.0's case mappings"
)

# Shared for the tests and tools; linked into kvasir-node for the package,
# which ships no library of Kvasir's.
if(KVASIR_SYSTEM_DANNAN)
  set(kvasir_library_type STATIC)
else()
  set(kvasir_library_type SHARED)
endif()
add_library(kvasir ${kvasir_library_type}
  ${kvasir_ducet_table}
  ${kvasir_general_ci_table}
  ${kvasir_casing9_table}
  ${kvasir_casing3_table}
  catalog/catalog.cpp
  catalog/table.cpp
  exec/aggregate.cpp
  exec/cast.cpp
  exec/casing.cpp
  exec/collation.cpp
  exec/date_functions.cpp
  exec/interval.cpp
  exec/format.cpp
  exec/time_arithmetic.cpp
  exec/ddl.cpp
  exec/like.cpp
  exec/decimal.cpp
  exec/expression.cpp
  exec/functions.cpp
  exec/information_schema.cpp
  exec/session.cpp
  exec/rows.cpp
  exec/store.cpp
  exec/temporal.cpp
  exec/variables.cpp
  protocol/caching_sha2.cpp
  protocol/codec.cpp
  protocol/messages.cpp
  protocol/packet.cpp
  protocol/statements.cpp
  server/accounts.cpp
  server/commands.cpp
  server/connection.cpp
  server/node.cpp
  server/node_settings.cpp
  server/pool_checker.cpp
  server/sign_in.cpp
  server/statistics.cpp
  server/stream_transport.cpp
  sql/keywords.cpp
  sql/lexer.cpp
  sql/parser.cpp
  txn/range_commit.cpp
  txn/range_proposer.cpp
  txn/store.cpp
  version.cpp
)
target_include_directories(kvasir
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(kvasir
  PUBLIC dannan::dannan Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE kvasir_warnings
)
set_target_properties(kvasir PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} POSITION_INDEPENDENT_CODE ON)
