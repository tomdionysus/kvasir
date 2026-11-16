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

add_library(kvasir SHARED
  ${kvasir_ducet_table}
  ${kvasir_general_ci_table}
  catalog/catalog.cpp
  catalog/table.cpp
  exec/collation.cpp
  exec/ddl.cpp
  exec/like.cpp
  exec/decimal.cpp
  exec/expression.cpp
  exec/session.cpp
  exec/rows.cpp
  exec/store.cpp
  exec/variables.cpp
  protocol/caching_sha2.cpp
  protocol/codec.cpp
  protocol/messages.cpp
  protocol/packet.cpp
  server/accounts.cpp
  server/commands.cpp
  server/connection.cpp
  server/node_settings.cpp
  server/pool_checker.cpp
  server/sign_in.cpp
  server/stream_transport.cpp
  sql/keywords.cpp
  sql/lexer.cpp
  sql/parser.cpp
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
set_target_properties(kvasir PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
