// Offline metadata/properties queries for arbitrary URLs. This mirrors
// Cog's generic reader facades but returns tuxedo's JSON shapes.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace tuxedo {

bool read_metadata_for_url(const std::string &url, nlohmann::json &out);
bool read_properties_for_url(const std::string &url, nlohmann::json &out);

} // namespace tuxedo
