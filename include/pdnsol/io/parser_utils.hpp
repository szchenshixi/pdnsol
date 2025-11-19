#pragma once

#include <string>
#include <vector>

#include "pdnsol/common.hpp"
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
    IdString mId;      // canonical node id (with GND mapping)
    int32_t mNet = -1; // e.g. from "n<net>_x_y"
    double mXMicros;   // in micrometers
    double mYMicros;   // in micrometers
};

// Map a node name to ParsedNode, using your PDN convention:
//
//   0 / gnd / GND   -> GND node
//   n<net>_<x>_<y>  -> PDN node with net-index and coordinates
//   others          -> general node; netIndex/x/y left unset
//
// coordToMircoScale converts x/y into meters (UM => 1, MM => 1e3).
ParsedNode parsePdNodeName(const std::string& raw, int32_t coordToMircoScale);

// True if this looks like a package node (e.g. "_X_n3_0_0")
bool isPackageNodeName(const std::string& name);

} // namespace pdnsol