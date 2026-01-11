#pragma once

#include <string>

#include "pdnsol/common.hpp"

namespace pdnsol {
bool integrityCheck(const Json& configJ, const std::string& filePath);
} // namespace pdnsol