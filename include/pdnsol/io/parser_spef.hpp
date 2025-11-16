#pragma once

#include <istream>
#include <string>

#include "pdnsol/common.hpp"
#include "pdnsol/struct/circuit.hpp"

namespace pdnsol {
// Parse SPEF from a stream into a CircuitGraph.
// - coordUnit controls how numeric coordinates in PDN-style node names
//   (n<net>_<x>_<y>) are converted to meters.
CircuitGraph parseSpef(std::istream& in,
                       CircuitGraph::Unit coordUnit = CircuitGraph::UM);

// Convenience overload for file path.
CircuitGraph parseSpefFile(const std::string& path,
                           CircuitGraph::Unit coordUnit = CircuitGraph::UM);
} // namespace pdnsol