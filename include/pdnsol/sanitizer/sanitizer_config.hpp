#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace pdnsol {
using Json = nlohmann::json;
bool integrityCheck(const Json& configJ, const std::string& filePath);
} // namespace pdnsol