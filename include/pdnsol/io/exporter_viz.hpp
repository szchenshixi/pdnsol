#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "pdnsol/struct/circuit.hpp"

namespace pdnsol {
using Json = nlohmann::json;
void exportCircuitGraphForVizJson(const CircuitGraph& c,
                                  const std::string&  outPath);
} // namespace pdnsol