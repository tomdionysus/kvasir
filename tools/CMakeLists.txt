#
# Kvasir - The Distributed Relational Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# kvasir-node: a node that serves the MySQL protocol.
add_executable(kvasir-node kvasir_node.cpp)
target_link_libraries(kvasir-node PRIVATE kvasir kvasir_warnings)
