#include "pdnsol/io/parser_spef.hpp"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

#include "pdnsol/io/parser_utils.hpp"

namespace {
using namespace pdnsol;

// -------------------------
// Helpers
// -------------------------

std::string unquote(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Resolve a SPEF name token with the name map.
// Token might be:
//   * "<rawName>"
//   * "\"raw name with spaces\""
//   * "*12" (name-map id)
//   * "*12:A" (name-map id plus delimiter and pin)
std::string
resolveSpefName(const std::string&                          token,
                const std::unordered_map<int, std::string>& nameMap) {
    if (token.empty()) return token;

    // Name-map reference?
    if (token[0] == '*' && token.size() > 1 &&
        std::isdigit(static_cast<unsigned char>(token[1]))) {
        // Split on first non-digit
        std::size_t pos = 1;
        while (pos < token.size() &&
               std::isdigit(static_cast<unsigned char>(token[pos]))) {
            ++pos;
        }
        int id = 0;
        try {
            id = std::stoi(token.substr(1, pos - 1));
        } catch (...) {
            // Fallback: treat as literal token
            return unquote(token);
        }

        auto it = nameMap.find(id);
        if (it == nameMap.end()) {
            // Unknown id; treat as literal
            return unquote(token);
        }

        std::string base = it->second; // resolved from map
        if (pos < token.size()) {
            // Remaining suffix (e.g., ":A")
            base += token.substr(pos);
        }
        return unquote(base);
    }

    return unquote(token);
}

// Parse *R_UNIT line to get resistance scale [ohms per numeric unit].
double parseRUnitScale(const std::vector<std::string>& tokens) {
    // Format: *R_UNIT <scale> <UNIT>
    if (tokens.size() < 3) return 1.0;

    double scale = 1.0;
    try {
        scale = std::stod(tokens[1]);
    } catch (...) {
        scale = 1.0;
    }

    std::string unit = toLower(tokens[2]);
    double      base = 1.0;
    if (unit == "ohm") base = 1.0;
    else if (unit == "kohm") base = 1e3;
    else if (unit == "mohm") base = 1e6;
    else base = 1.0; // unknown, assume ohm

    return scale * base;
}

// -------------------------
// Parser state
// -------------------------

enum class TopSection {
    NONE,
    NAME_MAP,
    PORTS,
    NET // inside some *D_NET ... *END block
};

enum class NetSubSection { NONE, CONN, CAP, RES };

struct SpefState {
    TopSection    topSection = TopSection::NONE;
    NetSubSection netSection = NetSubSection::NONE;

    // Name mapping *id -> string
    std::unordered_map<int, std::string> nameMap;

    // Current net context
    bool        inNet = false;
    std::string currentNetName; // resolved textual net name
    double      currentNetTotalCap = 0.0;

    // Resistance unit scaling to ohms
    double rUnitScale = 1.0;
};

// For each *D_NET, push a SectionMeta entry if desired.
void recordNetSection(CircuitGraph& circ, const SpefState& st) {
    SectionMeta meta;
    meta.type    = IdString("net");
    meta.name    = IdString(st.currentNetName);
    meta.net     = IdString(st.currentNetName);
    meta.fromNet = IdString(""); // not used
    meta.toNet   = IdString(""); // not used
    meta.raw     = "";           // could store full "*D_NET" line if needed
    circ.mSections.push_back(std::move(meta));
}

// Create MetalRes from SPEF *RES line.
void addResistorFromSpef(CircuitGraph& circ, const SpefState& st,
                         const std::vector<std::string>& tokens,
                         int32_t                         coordToMircoScale) {
    // Format: <idx> <node1> <node2> <R>
    if (tokens.size() < 4) {
        throw std::runtime_error("SPEF *RES entry has fewer than 4 tokens");
    }

    const std::string& idxStr = tokens[0];
    const std::string& n1Tok  = tokens[1];
    const std::string& n2Tok  = tokens[2];
    const std::string& rTok   = tokens[3];

    std::string n1Name = resolveSpefName(n1Tok, st.nameMap);
    std::string n2Name = resolveSpefName(n2Tok, st.nameMap);

    double rVal = 0.0;
    try {
        rVal = std::stod(rTok) * st.rUnitScale; // convert to ohms
    } catch (const std::exception& e) {
        throw std::runtime_error(
          std::string("Failed to parse SPEF resistor value '") + rTok +
          "': " + e.what());
    }

    ParsedNode pn1 = parsePdNodeName(n1Name, coordToMircoScale);
    ParsedNode pn2 = parsePdNodeName(n2Name, coordToMircoScale);

    circ.ensureNode(pn1.mId, pn1.mNet, pn1.mXMicros, pn1.mYMicros);
    circ.ensureNode(pn2.mId, pn2.mNet, pn2.mXMicros, pn2.mYMicros);

    MetalRes    res;
    // Unique-ish name: R_<net>_<idx>
    std::string rName = "R_" + st.currentNetName + "_" + idxStr;
    res.name          = IdString(rName);
    res.n1            = pn1.mId;
    res.n2            = pn2.mId;

    int32_t netIndex = -1;
    if (pn1.mNet >= 0) netIndex = pn1.mNet;
    else if (pn2.mNet >= 0) netIndex = pn2.mNet;
    res.net = NetId(netIndex);

    res.R = rVal;

    circ.mMetalResistors.push_back(std::move(res));
}

// For CAP entries: only ensure nodes exist; ignore capacitance numeric for DC.
void ensureNodesFromCap(CircuitGraph& circ, const SpefState& st,
                        const std::vector<std::string>& tokens,
                        int32_t                         coordToMircoScale) {
    // Ground cap:   <idx> <node> <C>
    // Coupling cap: <idx> <node1> <node2> <C>
    if (tokens.size() < 3) {
        return;
    }

    if (tokens.size() == 3) {
        // <idx> <node> <C>
        std::string nodeTok  = tokens[1];
        std::string nodeName = resolveSpefName(nodeTok, st.nameMap);
        ParsedNode  pn       = parsePdNodeName(nodeName, coordToMircoScale);
        circ.ensureNode(pn.mId, pn.mNet, pn.mXMicros, pn.mYMicros);
    } else if (tokens.size() >= 4) {
        // <idx> <node1> <node2> <C>
        std::string n1Tok = tokens[1];
        std::string n2Tok = tokens[2];

        std::string n1Name = resolveSpefName(n1Tok, st.nameMap);
        std::string n2Name = resolveSpefName(n2Tok, st.nameMap);

        ParsedNode pn1 = parsePdNodeName(n1Name, coordToMircoScale);
        ParsedNode pn2 = parsePdNodeName(n2Name, coordToMircoScale);

        circ.ensureNode(pn1.mId, pn1.mNet, pn1.mXMicros, pn1.mYMicros);
        circ.ensureNode(pn2.mId, pn2.mNet, pn2.mXMicros, pn2.mYMicros);
    }
}

// Handle *NAME_MAP entry like: "*12 someName"
void handleNameMapEntry(SpefState& st, const std::string& line) {
    // Strip leading/trailing spaces
    std::string stripped = trim(line);
    if (stripped.empty() || stripped[0] != '*') return;

    auto tokens = splitWhitespace(stripped);
    if (tokens.size() < 2) return;

    const std::string& idTok   = tokens[0];
    const std::string& nameTok = tokens[1];

    if (idTok.size() < 2 || idTok[0] != '*' ||
        !std::isdigit(static_cast<unsigned char>(idTok[1]))) {
        return;
    }

    int id = 0;
    try {
        id = std::stoi(idTok.substr(1));
    } catch (...) {
        return;
    }

    st.nameMap[id] = unquote(nameTok);
}

// Handle *CONN line: we only ensure nodes exist.
void handleConnLine(CircuitGraph& circ, const SpefState& st,
                    const std::vector<std::string>& tokens,
                    int32_t                         coordToMircoScale) {
    // Formats (simplified):
    // *P <node> <dir> ...
    // *I <inst:pin> <dir> ...
    // *D <internalNode> <dir> ...
    if (tokens.size() < 2) return;

    const std::string& typeTok = tokens[0];
    const std::string& nodeTok = tokens[1];

    if (typeTok.size() < 2 || typeTok[0] != '*') return;

    // Resolve name map
    std::string nodeName = resolveSpefName(nodeTok, st.nameMap);
    ParsedNode  pn       = parsePdNodeName(nodeName, coordToMircoScale);

    circ.ensureNode(pn.mId, pn.mNet, pn.mXMicros, pn.mYMicros);
}

} // anonymous namespace

namespace pdnsol {
// -------------------------
// Public API
// -------------------------

CircuitGraph parseSpef(std::istream& in, CircuitGraph::Unit coordUnit) {
    CircuitGraph circ;
    circ.mCoordinateUnit = coordUnit;

    // Unit is either UM or MM
    const int32_t coordToMircoScale =
      (coordUnit == CircuitGraph::UM) ? 1 : 1000;

    SpefState   st;
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(in, line)) {
        ++lineNumber;
        std::string stripped = trim(line);
        if (stripped.empty()) continue;

        // Lines beginning with '*' are section headers, name-map entries, or
        // connection entries
        if (stripped[0] == '*') {
            auto tokens = splitWhitespace(stripped);
            if (tokens.empty()) continue;

            const std::string& key = tokens[0];

            // Top-level headers / units / metadata
            if (iequals(key, "*spef")) {
                circ.mMetadata[IdString("SPEF_FORMAT")] = IdString(stripped);
                continue;
            }
            if (iequals(key, "*design")) {
                if (tokens.size() >= 2) {
                    circ.mMetadata[IdString("SPEF_DESIGN")] =
                      IdString(unquote(tokens[1]));
                }
                continue;
            }
            if (iequals(key, "*date")) {
                if (tokens.size() >= 2) {
                    circ.mMetadata[IdString("SPEF_DATE")] =
                      IdString(trim(stripped.substr(5)));
                }
                continue;
            }
            if (iequals(key, "*vendor")) {
                if (tokens.size() >= 2) {
                    circ.mMetadata[IdString("SPEF_VENDOR")] =
                      IdString(unquote(tokens[1]));
                }
                continue;
            }
            if (iequals(key, "*program")) {
                if (tokens.size() >= 2) {
                    circ.mMetadata[IdString("SPEF_PROGRAM")] =
                      IdString(unquote(tokens[1]));
                }
                continue;
            }
            if (iequals(key, "*version")) {
                if (tokens.size() >= 2) {
                    circ.mMetadata[IdString("SPEF_VERSION")] =
                      IdString(unquote(tokens[1]));
                }
                continue;
            }
            if (iequals(key, "*r_unit")) {
                st.rUnitScale = parseRUnitScale(tokens);
                circ.mMetadata[IdString("SPEF_R_UNIT")] = IdString(stripped);
                continue;
            }
            if (iequals(key, "*c_unit")) {
                circ.mMetadata[IdString("SPEF_C_UNIT")] = IdString(stripped);
                continue;
            }
            if (iequals(key, "*t_unit")) {
                circ.mMetadata[IdString("SPEF_T_UNIT")] = IdString(stripped);
                continue;
            }

            // Section changes
            if (iequals(key, "*name_map")) {
                st.topSection = TopSection::NAME_MAP;
                continue;
            }
            if (iequals(key, "*ports")) {
                st.topSection = TopSection::PORTS;
                continue;
            }

            // *D_NET <netName> <totalCap>
            if (iequals(key, "*d_net")) {
                if (tokens.size() < 3) {
                    throw std::runtime_error("Malformed *D_NET at line " +
                                             std::to_string(lineNumber));
                }

                st.topSection = TopSection::NET;
                st.netSection = NetSubSection::NONE;
                st.inNet      = true;

                std::string netTok      = tokens[1];
                std::string totalCapTok = tokens[2];

                st.currentNetName = resolveSpefName(netTok, st.nameMap);
                try {
                    st.currentNetTotalCap = std::stod(totalCapTok);
                } catch (...) {
                    st.currentNetTotalCap = 0.0;
                }

                recordNetSection(circ, st);
                continue;
            }

            // Within NAME_MAP: lines like "*12 foo"
            if (st.topSection == TopSection::NAME_MAP &&
                !iequals(key, "*name_map")) {
                handleNameMapEntry(st, stripped);
                continue;
            }

            // Inside a net: sub-section headers
            if (st.topSection == TopSection::NET && st.inNet) {
                if (iequals(key, "*conn")) {
                    st.netSection = NetSubSection::CONN;
                    continue;
                }
                if (iequals(key, "*cap")) {
                    st.netSection = NetSubSection::CAP;
                    continue;
                }
                if (iequals(key, "*res")) {
                    st.netSection = NetSubSection::RES;
                    continue;
                }
                if (iequals(key, "*end")) {
                    st.netSection = NetSubSection::NONE;
                    st.inNet      = false;
                    continue;
                }

                // Connection entries: *P, *I, *D
                if (st.netSection == NetSubSection::CONN) {
                    if (key.size() >= 2 && key[0] == '*' &&
                        (key[1] == 'P' || key[1] == 'I' || key[1] == 'D' ||
                         key[1] == 'p' || key[1] == 'i' || key[1] == 'd')) {
                        handleConnLine(circ, st, tokens, coordToMircoScale);
                        continue;
                    }
                }
            }

            // Other *something lines are ignored
            continue;
        } // if line starts with '*'

        // Non-'*' lines: these appear in *CAP and *RES sections
        if (st.topSection == TopSection::NET && st.inNet) {
            auto tokens = splitWhitespace(stripped);
            if (tokens.empty()) continue;

            if (st.netSection == NetSubSection::CAP) {
                ensureNodesFromCap(circ, st, tokens, coordToMircoScale);
            } else if (st.netSection == NetSubSection::RES) {
                addResistorFromSpef(circ, st, tokens, coordToMircoScale);
            } else {
                // Unexpected; ignore
            }
        }
    } // while getline

    return circ;
}

CircuitGraph parseSpefFile(const std::string& path,
                           CircuitGraph::Unit coordUnit) {
    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("Failed to open SPEF file: " + path);
    }
    return parseSpef(ifs, coordUnit);
}
} // namespace pdnsol
