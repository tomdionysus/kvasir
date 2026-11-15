#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The database. Public headers are kvasir/include/kvasir/*.h, included as
# "kvasir/x.h"; everything else is included from the repository root as
# "kvasir/<subsystem>/x.h".
add_library(kvasir SHARED
  protocol/caching_sha2.cpp
  protocol/codec.cpp
  protocol/messages.cpp
  protocol/packet.cpp
  server/accounts.cpp
  server/sign_in.cpp
  sql/lexer.cpp
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
