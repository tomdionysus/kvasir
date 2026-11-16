#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# kvasir-ducet: compiles Unicode's collation table into the library at build
# time (kvasir/CMakeLists.txt runs it). It links nothing of Kvasir's.
add_executable(kvasir-ducet kvasir_ducet.cpp)
target_link_libraries(kvasir-ducet PRIVATE kvasir_warnings)

# kvasir-general-ci: derives the general_ci weights from Unicode 3.0.0's
# character database at build time (ruling 33).
add_executable(kvasir-general-ci kvasir_general_ci.cpp)
target_link_libraries(kvasir-general-ci PRIVATE kvasir_warnings)

# kvasir-node: a node that serves the MySQL protocol.
add_executable(kvasir-node kvasir_node.cpp)
target_link_libraries(kvasir-node PRIVATE kvasir kvasir_warnings)
