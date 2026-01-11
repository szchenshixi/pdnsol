#pragma once

#include <string>

#include "pdnsol/common.hpp"
#include "pdnsol/struct/circuit.hpp"

namespace pdnsol {
void exportCircuitGraphForVizJson(const CircuitGraph& c,
                                  const std::string&  outPath);
} // namespace pdnsol