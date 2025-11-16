#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pdnsol/utils/id_string.hpp"

// Shared parsing utilities for SPICE and SPEF
namespace pdnsol {

// -------------------------
// Generic string helpers
// -------------------------

std::string ltrim(const std::string& s);
std::string rtrim(const std::string& s);
std::string trim(const std::string& s);
std::string toLower(std::string s);
bool iequals(const std::string& a, const std::string& b);
bool startsWithIgnoreCase(const std::string& s, const std::string& prefix);

std::vector<std::string> splitWhitespace(const std::string& line);
std::vector<std::string> splitOnChar(const std::string& s, char delim);

// -------------------------
// Node name parsing for PDN
// -------------------------

struct ParsedNode {
    IdString id;                   // canonical node id (with GND mapping)
    int32_t netIndex = -1;         // e.g. from "n<net>_x_y"
    std::optional<double> xMeters; // in meters
    std::optional<double> yMeters;
};

// Map a node name to ParsedNode, using your PDN convention:
//
//   0 / gnd / GND   -> GND node
//   n<net>_<x>_<y>  -> PDN node with net-index and coordinates
//   others          -> general node; netIndex/x/y left unset
//
// coordToMeterScale converts x/y into meters (UM => 1e-6, MM => 1e-3).
ParsedNode parsePdNodeName(const std::string& raw, double coordToMeterScale);

// True if this looks like a package node (e.g. "_X_n3_0_0")
bool isPackageNodeName(const std::string& name);

} // namespace pdnsol