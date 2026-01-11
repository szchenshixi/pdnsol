// SpiceParser.h
#pragma once

#include <istream>
#include <string>

#include "pdnsol/common.hpp"
#include "pdnsol/struct/circuit.hpp"

namespace pdnsol {
// Parse from an existing stream (e.g., std::ifstream).  coordUnit is the
// unit of the x/y numbers in node names (UM => microns, MM => millimeters).
CircuitGraph parseSpice(std::istream&      in,
                        CircuitGraph::Unit coordUnit = CircuitGraph::UM);

// Convenience: open a file and call parseSpice on it.
CircuitGraph parseSpiceFile(const std::string& path,
                            CircuitGraph::Unit coordUnit = CircuitGraph::UM);
} // namespace pdnsol