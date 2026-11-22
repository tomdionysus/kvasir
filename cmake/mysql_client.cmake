#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# A real MySQL client library, for the tests only (the maintainer, 2026-11-20
# and 21): libmysqlclient 8.4.13 and Connector/C++ 26.10.0's JDBC API, built
# from Oracle's source, pinned by URL and hash. Never linked into Kvasir.
#
# Both are built once into KVASIR_MYSQL_CLIENT_ROOT and found there by every
# later configure, so the build trees of one checkout share one build
# (tools/ci.sh points them all at build-ci/mysql-client). Only the client is
# built: no server, router or plugins. Needs libtirpc-dev and pkg-config (MySQL
# checks for rpc/rpc.h on every Linux build), no bison.
#
# Defines kvasir::mysql_jdbc, Connector/C++'s classic API over libmysqlclient,
# both static, and kvasir_mysql_client, the target that builds them.

include(ExternalProject)

set(KVASIR_MYSQL_CLIENT_ROOT "${PROJECT_BINARY_DIR}/mysql-client" CACHE PATH "Where the tests' MySQL client libraries are built once and found after")
# The jobs the two builds run with; a shared build node gives 1 or 2.
set(KVASIR_MYSQL_CLIENT_JOBS "2" CACHE STRING "Parallel jobs for the MySQL client libraries' builds")

set(kvasir_mysql_version "8.4.13")
set(kvasir_mysql_url "https://cdn.mysql.com/Downloads/MySQL-8.4/mysql-${kvasir_mysql_version}.tar.gz")
set(kvasir_mysql_sha256 "f6ddc7f7fb6462bab8b533b13c06af3fff57fa9ca12259b81ab13cd9930b9d58")
set(kvasir_concpp_version "26.10.0")
set(kvasir_concpp_url "https://cdn.mysql.com/Downloads/Connector-C++/mysql-connector-c++-${kvasir_concpp_version}-src.tar.gz")
set(kvasir_concpp_sha256 "25d83a76164da29592b4e029c6e771cd05139453be2f8fc206cc7c73d19fa9b2")

# One directory per pair of versions: a moved pin builds afresh.
set(kvasir_client_prefix "${KVASIR_MYSQL_CLIENT_ROOT}/mysql-${kvasir_mysql_version}-concpp-${kvasir_concpp_version}")
set(kvasir_mysqlclient_dir "${kvasir_client_prefix}/libmysqlclient")
set(kvasir_concpp_dir "${kvasir_client_prefix}/connector")
set(kvasir_mysqlclient_library "${kvasir_mysqlclient_dir}/lib/libmysqlclient.a")
# The driver's own archive. Connector/C++'s installed libmysqlcppconn-static.a
# should hold it merged with libmysqlclient, but 26.10.0's merge leaves the
# driver's objects out (measured 2026-11-22: only libmysqlclient's are there).
set(kvasir_jdbc_library "${kvasir_concpp_dir}/lib/libjdbc.a")

add_custom_target(kvasir_mysql_client)
if(NOT EXISTS "${kvasir_jdbc_library}")
  set(kvasir_build_jobs --parallel ${KVASIR_MYSQL_CLIENT_JOBS})
  ExternalProject_Add(kvasir_mysqlclient_build
    URL "${kvasir_mysql_url}"
    URL_HASH SHA256=${kvasir_mysql_sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${PROJECT_BINARY_DIR}/mysql-client-build/libmysqlclient"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=${kvasir_mysqlclient_dir}
      -DWITHOUT_SERVER=ON
      -DWITH_UNIT_TESTS=OFF
      -DWITH_ROUTER=OFF
      -DWITH_SSL=system
      -DWITH_ZLIB=bundled
      -DWITH_ZSTD=bundled
      -DWITH_EDITLINE=bundled
      -DWITH_FIDO=none
      -DWITH_AUTHENTICATION_LDAP=OFF
      -DWITH_AUTHENTICATION_KERBEROS=OFF
      -DWITH_AUTHENTICATION_CLIENT_PLUGINS=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target mysqlclient ${kvasir_build_jobs}
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --component Development
    BUILD_BYPRODUCTS "${kvasir_mysqlclient_library}"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON
  )
  ExternalProject_Add(kvasir_concpp_build
    DEPENDS kvasir_mysqlclient_build
    URL "${kvasir_concpp_url}"
    URL_HASH SHA256=${kvasir_concpp_sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PREFIX "${PROJECT_BINARY_DIR}/mysql-client-build/connector"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=${kvasir_concpp_dir}
      -DCMAKE_INSTALL_LIBDIR=lib
      -DWITH_JDBC=ON
      -DWITH_SSL=system
      -DMYSQL_DIR=${kvasir_mysqlclient_dir}
      -DMYSQLCLIENT_STATIC_LINKING=ON
      -DBUILD_STATIC=ON
    # The classic API alone, not the X DevAPI.
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target connector-jdbc ${kvasir_build_jobs}
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --component JDBCDev
      COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/jdbc/driver/libjdbc.a ${kvasir_jdbc_library}
    BUILD_BYPRODUCTS "${kvasir_jdbc_library}"
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON
  )
  add_dependencies(kvasir_mysql_client kvasir_concpp_build)
endif()

# An imported target's include directory must exist when it is configured.
file(MAKE_DIRECTORY "${kvasir_mysqlclient_dir}/include" "${kvasir_concpp_dir}/include")

add_library(kvasir_mysqlclient STATIC IMPORTED GLOBAL)
set_target_properties(kvasir_mysqlclient PROPERTIES
  IMPORTED_LOCATION "${kvasir_mysqlclient_library}"
  INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto;resolv;${CMAKE_DL_LIBS};Threads::Threads"
)
add_library(kvasir_mysql_jdbc STATIC IMPORTED GLOBAL)
set_target_properties(kvasir_mysql_jdbc PROPERTIES
  IMPORTED_LOCATION "${kvasir_jdbc_library}"
  # As system headers: their warnings are not Kvasir's.
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${kvasir_concpp_dir}/include"
  INTERFACE_INCLUDE_DIRECTORIES "${kvasir_concpp_dir}/include"
  INTERFACE_COMPILE_DEFINITIONS STATIC_CONCPP
  INTERFACE_LINK_LIBRARIES kvasir_mysqlclient
)
add_library(kvasir::mysql_jdbc ALIAS kvasir_mysql_jdbc)
# A target linking kvasir::mysql_jdbc also depends on kvasir_mysql_client,
# which builds the libraries when the root does not hold them yet.
