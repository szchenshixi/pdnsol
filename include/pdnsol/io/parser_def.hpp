#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "pdnsol/struct/circuit.hpp"

namespace pdnsol {

/**
 * Parse a DEF file from a stream and augment the given CircuitGraph
 * with node coordinates inferred from:
 *
 *  - PINS section (top-level pins / bumps)
 *  - COMPONENTS section (instance placements; inst/pin nodes placed at inst
 * origin)
 *
 * The internal nodes (without a direct DEF anchor) can optionally be
 * assigned approximate positions by propagating coordinates along the
 * resistive graph.
 *
 * Throws std::runtime_error on parse failures.
 */
void augmentCircuitGraphWithDef(std::istream& defIn, CircuitGraph& circ,
                                bool propagateInternalNodes = true);

/**
 * Convenience wrapper that opens the file and calls
 * augmentCircuitGraphWithDef.
 */
void augmentCircuitGraphWithDefFile(const std::string& path,
                                    CircuitGraph& circ,
                                    bool propagateInternalNodes = true);

} // namespace pdnsol