#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(kvasir_tests
  catalog_catalog_test.cpp
  catalog_table_test.cpp
  differential_test.cpp
  exec_casing_test.cpp
  exec_collation_test.cpp
  exec_decimal_test.cpp
  exec_expression_test.cpp
  exec_rows_test.cpp
  exec_session_test.cpp
  exec_variables_test.cpp
  fuzz_smoke_test.cpp
  protocol_caching_sha2_test.cpp
  protocol_codec_test.cpp
  protocol_messages_test.cpp
  protocol_packet_test.cpp
  protocol_statements_test.cpp
  server_accounts_test.cpp
  server_commands_test.cpp
  server_connection_test.cpp
  server_node_settings_test.cpp
  server_pool_checker_test.cpp
  server_sign_in_test.cpp
  sql_lexer_test.cpp
  sql_parser_test.cpp
  txn_commit_sim_test.cpp
  txn_range_commit_test.cpp
  txn_store_test.cpp
  version_test.cpp
  # The fuzz targets, registered with Dannan's harness, for fuzz_smoke_test.
  ../fuzz/execute_fuzz.cpp
  ../fuzz/lexer_fuzz.cpp
  ../fuzz/packet_fuzz.cpp
  ../fuzz/parser_fuzz.cpp
)
# Dannan's test package: the fuzz harness and the simulation checkers, with
# its installed simulator fixtures (dannan/test/fixtures/sim/).
target_link_libraries(kvasir_tests PRIVATE kvasir dannan::testing GTest::gtest_main kvasir_warnings)
# A node driven by a real MySQL client library (cmake/mysql_client.cmake).
if(KVASIR_MYSQL_CLIENT_TESTS)
  target_sources(kvasir_tests PRIVATE server_mysql_client_test.cpp)
  target_link_libraries(kvasir_tests PRIVATE kvasir::mysql_jdbc)
  add_dependencies(kvasir_tests kvasir_mysql_client)
endif()
# Dannan's fixtures write only inside Kvasir's build tree.
target_compile_definitions(kvasir_tests PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp"
                                                  KVASIR_DIFFERENTIAL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/differential")
