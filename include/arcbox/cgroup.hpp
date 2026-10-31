#pragma once

#include <string>
#include <vector>

#include "arcbox/config.hpp"

namespace arcbox {

bool cgroup_available();
int run_in_cgroup(const Config& cfg,
                  const std::vector<std::string>& argv,
                  std::string& error);

} // namespace arcbox
