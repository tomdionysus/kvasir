//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// kvasir-node: one Kvasir node. At the skeleton stage it reads its settings
// and reports its version; it serves nothing yet, and says so rather than
// listening on a port it cannot answer.
//
// Every setting is also a key in a YAML file (--config PATH, $KVASIR_CONFIG, or
// /usr/local/etc/kvasir/config.yaml when it exists), in secrets.yaml beside
// it, or a KVASIR_* variable, the flags winning.
//
//   kvasir-node --version

#include <iostream>
#include <string>
#include <vector>

#include "dannan/config/config.h"
#include "kvasir/version.h"

namespace {

dannan::config::Reader reader() {
  using dannan::config::Setting;
  dannan::config::Reader r;
  r.config_variable = "KVASIR_CONFIG";
  r.default_path = "/usr/local/etc/kvasir/config.yaml";
  r.settings = {
      Setting{"version", Setting::Kind::toggle, "--version", "", "print the version and exit"},
  };
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto settings = reader().read(std::vector<std::string>(argv + 1, argv + argc));
    if (settings.toggle("version").value_or(false)) {
      std::cout << "kvasir-node " << kvasir::version << std::endl;
      return 0;
    }
    std::cerr << "kvasir-node: " << kvasir::version << " serves nothing yet; only --version is built" << std::endl;
    return 2;
  } catch (const dannan::config::ConfigError& e) {
    std::cerr << "kvasir-node: " << e.what() << std::endl;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "kvasir-node: " << e.what() << std::endl;
    return 1;
  }
}
