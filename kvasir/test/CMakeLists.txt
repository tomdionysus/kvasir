#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(kvasir_tests
  differential_test.cpp
  exec_collation_test.cpp
  exec_decimal_test.cpp
  exec_expression_test.cpp
  exec_session_test.cpp
  exec_variables_test.cpp
  protocol_caching_sha2_test.cpp
  protocol_codec_test.cpp
  protocol_messages_test.cpp
  protocol_packet_test.cpp
  server_accounts_test.cpp
  server_commands_test.cpp
  server_connection_test.cpp
  server_node_settings_test.cpp
  server_sign_in_test.cpp
  sql_lexer_test.cpp
  sql_parser_test.cpp
  version_test.cpp
)
target_link_libraries(kvasir_tests PRIVATE kvasir GTest::gtest_main kvasir_warnings)
# Dannan's fixtures write only inside Kvasir's build tree.
target_compile_definitions(kvasir_tests PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp"
                                                  KVASIR_DIFFERENTIAL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/differential")
