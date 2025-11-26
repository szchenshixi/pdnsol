#include "pdnsol/io/parser_spice.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "pdnsol/io/parser_utils.hpp"
#include "pdnsol/utils/logging.hpp"

// -------------------------
// Small string utilities
// -------------------------

namespace {
using namespace pdnsol;

// Parse SPICE numeric with suffix: f, p, n, u, m, k, meg, g, t
// "0.3125m" -> 3.125e-4, "1k" -> 1000, "5" -> 5
double parseSpiceNumber(const std::string& token) {
    std::string t = trim(token);
    if (t.empty()) {
        throw std::runtime_error("Empty numeric token in SPICE netlist");
    }

    std::string lower = toLower(t);
    double      scale = 1.0;

    // Handle multi-char suffix 'meg'
    if (lower.size() > 3 && lower.substr(lower.size() - 3) == "meg") {
        scale = 1e6;
        t     = t.substr(0, t.size() - 3);
    } else {
        char last = static_cast<char>(
          std::tolower(static_cast<unsigned char>(lower.back())));
        switch (last) {
        case 'f':
            scale = 1e-15;
            t.pop_back();
            break;
        case 'p':
            scale = 1e-12;
            t.pop_back();
            break;
        case 'n':
            scale = 1e-9;
            t.pop_back();
            break;
        case 'u':
            scale = 1e-6;
            t.pop_back();
            break;
        case 'm':
            scale = 1e-3;
            t.pop_back();
            break;
        case 'k':
            scale = 1e3;
            t.pop_back();
            break;
        case 'g':
            scale = 1e9;
            t.pop_back();
            break;
        case 't':
            scale = 1e12;
            t.pop_back();
            break;
        default: break; // no suffix
        }
    }

    double base = 0.0;
    try {
        base = std::stod(t);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to parse numeric token '" + token +
                                 "': " + e.what());
    }
    return base * scale;
}

// -------------------------
// Node-name parsing
// -------------------------

struct ParsedNode {
    IdString              id; // canonical node id (with GND mapping)
    int32_t               netIndex = -1; // from "n<net>_x_y" (if available)
    std::optional<double> x;             // micrometer
    std::optional<double> y;             // micrometer
};

// Map SPICE node name to ParsedNode.  Node forms:
//   0              -> global ground
//   gnd / GND      -> ground
//   n<net>_x_y     -> PDN node with net-index and coordinates
//   anything else  -> arbitrary node (package, etc.)
ParsedNode parseNodeName(const std::string& raw, int32_t coordToMicroScale) {
    ParsedNode out;

    // Ground mapping
    if (raw == "0" || iequals(raw, "gnd")) {
        out.id = IdString("GND");
        return out;
    }

    out.id = IdString(raw);

    // Check for Package node: _X_n<net>_<x>_<y>
    if (raw.size() > 4 && raw.substr(0, 3) == "_X_") {
        std::string rest = raw.substr(3); // drop leading '_X_'
        if (rest.size() > 1 && (rest[0] == 'n' || rest[0] == 'N')) {
            std::string geometryPart = rest.substr(1); // drop leading 'n'
            auto        parts        = splitOnChar(geometryPart, '_');
            if (parts.size() == 3) {
                try {
                    int32_t net  = static_cast<int32_t>(std::stoi(parts[0]));
                    double  x    = std::stod(parts[1]);
                    double  y    = std::stod(parts[2]);
                    out.netIndex = -net; // package nodes feature negative net
                    out.x        = x * coordToMicroScale;
                    out.y        = y * coordToMicroScale;
                } catch (...) {
                    // If parsing fails, leave netIndex/x/y as defaults.
                }
            }
        }
    }

    // Geometry node: n<net>_<x>_<y>
    if (raw.size() > 1 && (raw[0] == 'n' || raw[0] == 'N')) {
        std::string rest  = raw.substr(1); // drop leading 'n'
        auto        parts = splitOnChar(rest, '_');
        if (parts.size() == 3) {
            try {
                int32_t net  = static_cast<int32_t>(std::stoi(parts[0]));
                double  x    = std::stod(parts[1]);
                double  y    = std::stod(parts[2]);
                out.netIndex = net;
                out.x        = x * coordToMicroScale;
                out.y        = y * coordToMicroScale;
            } catch (...) {
                // If parsing fails, leave netIndex/x/y as defaults.
            }
        }
    }

    return out;
}

// True if this looks like a package node name (e.g. "_X_n3_0_0")
bool isPackageNodeName(const std::string& name) {
    return !name.empty() && name[0] == '_';
}

// -------------------------
// Parser state
// -------------------------

enum class SectionKind { NONE, METAL_LAYER, VIA_SECTION };

struct ParseState {
    SectionKind current = SectionKind::NONE;

    // For METAL_LAYER
    IdString layerName;
    IdString layerNetName; // e.g., "VDD", "GND"
    int32_t  layerNetIndex = -1;

    // For VIA_SECTION
    int32_t viaFromNet = -1;
    int32_t viaToNet   = -1;
};

// Parse a comment line that may contain "* layer:" or "* vias from:"
// and update CircuitGraph::mSections and parser state.
void parseCommentMeta(
  const std::string& lineBody, // already without leading '*'
  CircuitGraph& circ, ParseState& st) {
    std::string content = trim(lineBody);
    if (content.empty())
        return;

    std::string lower = toLower(content);

    // Example: "* layer: M1,VDD net: 1"
    if (startsWithIgnoreCase(lower, "layer:")) {
        std::string rest = trim(content.substr(std::string("layer:").size()));
        // rest: "M1,VDD net: 1"
        auto        tokens = splitWhitespace(rest);
        if (tokens.empty())
            return;

        // First token: "<layerName>,<netName>"
        auto        nameParts = splitOnChar(tokens[0], ',');
        std::string layerNameStr =
          nameParts.size() > 0 ? trim(nameParts[0]) : "";
        std::string netNameStr =
          nameParts.size() > 1 ? trim(nameParts[1]) : "";

        int32_t netIndex = -1;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            std::string t   = toLower(tokens[i]);
            auto        pos = t.find("net:");
            if (pos != std::string::npos) {
                std::string numStr = t.substr(pos + 4); // after "net:"
                numStr             = trim(numStr);
                if (numStr.empty() && i + 1 < tokens.size()) {
                    numStr = tokens[i + 1];
                }
                try {
                    netIndex = static_cast<int32_t>(std::stoi(numStr));
                } catch (...) {
                    netIndex = -1;
                }
                break;
            }
        }

        st.current       = SectionKind::METAL_LAYER;
        st.layerName     = IdString(layerNameStr);
        st.layerNetName  = IdString(netNameStr);
        st.layerNetIndex = netIndex;
        st.viaFromNet    = -1;
        st.viaToNet      = -1;

        SectionMeta meta;
        meta.mType    = IdString("layer");
        meta.mName    = IdString(layerNameStr);
        meta.mNet     = IdString(netNameStr);               // textual net name
        meta.mFromNet = IdString(std::to_string(netIndex)); // store index here
        meta.mToNet   = IdString("");                       // unused for layer
        meta.mRaw     = IdString(content);
        circ.mSections.push_back(std::move(meta));
        return;
    }

    // Example: "* vias from: 1 to 3"
    if (startsWithIgnoreCase(lower, "vias from:")) {
        std::string rest =
          trim(content.substr(std::string("vias from:").size()));
        auto    tokens  = splitWhitespace(rest);
        int32_t fromNet = -1;
        int32_t toNet   = -1;

        if (!tokens.empty()) {
            try {
                fromNet = static_cast<int32_t>(std::stoi(tokens[0]));
            } catch (...) {
                fromNet = -1;
            }
        }
        for (std::size_t i = 1; i + 1 < tokens.size(); ++i) {
            if (iequals(tokens[i], "to")) {
                try {
                    toNet = static_cast<int32_t>(std::stoi(tokens[i + 1]));
                } catch (...) {
                    toNet = -1;
                }
                break;
            }
        }

        st.current    = SectionKind::VIA_SECTION;
        st.viaFromNet = fromNet;
        st.viaToNet   = toNet;

        SectionMeta meta;
        meta.mType    = IdString("vias");
        meta.mName    = IdString(""); // no explicit name
        meta.mNet     = IdString(""); // unused
        meta.mFromNet = IdString(std::to_string(fromNet));
        meta.mToNet   = IdString(std::to_string(toNet));
        meta.mRaw     = IdString(content);
        circ.mSections.push_back(std::move(meta));
        return;
    }

    // Other comments do not affect parser state.
}

// -------------------------
// Element parsing helpers
// -------------------------

void parseResistor(const std::vector<std::string>& tokens, CircuitGraph& circ,
                   const ParseState& st, int32_t coordToMircoScale) {
    if (tokens.size() < 4) {
        PDN_FATAL("Resistor line has fewer than 4 tokens: '%s'",
                  (tokens.empty() ? std::string{} : tokens[0]).c_str());
    }

    const std::string& name  = tokens[0];
    const std::string& n1Str = tokens[1];
    const std::string& n2Str = tokens[2];
    const std::string& val   = tokens[3];

    double R = parseSpiceNumber(val);

    ParsedNode pn1 = parseNodeName(n1Str, coordToMircoScale);
    ParsedNode pn2 = parseNodeName(n2Str, coordToMircoScale);

    circ.ensureNode(pn1.id, pn1.netIndex, pn1.x, pn1.y);
    circ.ensureNode(pn2.id, pn2.netIndex, pn2.x, pn2.y);

    char firstChar =
      static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (firstChar != 'R') {
        // Should not happen if caller checks the first token.
        return;
    }

    if (st.current == SectionKind::VIA_SECTION) {
        // Via implemented as resistor
        ViaRes via;
        via.mName = IdString(name);
        via.mN1   = pn1.id;
        via.mN2   = pn2.id;
        via.mR    = R;
        circ.mViaResistors.push_back(std::move(via));
    } else if (st.current == SectionKind::METAL_LAYER) {
        // On-chip metal
        MetalRes mr;
        mr.mName = IdString(name);
        mr.mN1   = pn1.id;
        mr.mN2   = pn2.id;

        int32_t netIdx = st.layerNetIndex;
        if (netIdx < 0) {
            if (pn1.netIndex >= 0)
                netIdx = pn1.netIndex;
            else if (pn2.netIndex >= 0)
                netIdx = pn2.netIndex;
        }
        mr.mNet = netIdx;
        mr.mR   = R;
        circ.mMetalResistors.push_back(std::move(mr));
    } else {
        // Outside known layer/via sections -> treat as package resistor
        PkgRes pr;
        pr.mName = IdString(name);
        pr.mN1   = pn1.id;
        pr.mN2   = pn2.id;
        pr.mR    = R;
        circ.mPkgResistors.push_back(std::move(pr));
    }
}

void parseVoltageSource(const std::vector<std::string>& tokens,
                        CircuitGraph& circ, const ParseState& st,
                        int32_t coordToMircoScale, bool& seenGlobalVsrc) {
    if (tokens.size() < 4) {
        PDN_FATAL("Voltage source line has fewer than 4 tokens: '%s'",
                  (tokens.empty() ? std::string{} : tokens[0]).c_str());
    }

    const std::string& name  = tokens[0];
    const std::string& n1Str = tokens[1];
    const std::string& n2Str = tokens[2];
    const std::string& val   = tokens[3];

    double V = parseSpiceNumber(val);

    ParsedNode pn1 = parseNodeName(n1Str, coordToMircoScale);
    ParsedNode pn2 = parseNodeName(n2Str, coordToMircoScale);

    circ.ensureNode(pn1.id, pn1.netIndex, pn1.x, pn1.y);
    circ.ensureNode(pn2.id, pn2.netIndex, pn2.x, pn2.y);

    if (st.current == SectionKind::VIA_SECTION) {
        // DEBUG: Vias implemented as a constant resistor no mater what
        ViaRes src;
        src.mName = IdString(name);
        src.mN1   = pn1.id;
        src.mN2   = pn2.id;
        src.mR    = 0.1; // Ohm
        circ.mViaResistors.push_back(std::move(src));
        return;
    }

    Vsrc src;
    src.mName     = IdString(name);
    src.mFromNode = pn1.id;
    src.mToNode   = pn2.id;
    src.mV        = V;
    src.mType     = Vsrc::OTHER;
    bool connectsToGnd =
      (pn1.id == IdString("GND") || pn2.id == IdString("GND"));

    bool touchesPackage =
      (isPackageNodeName(n1Str) || isPackageNodeName(n2Str));

    if (!seenGlobalVsrc && connectsToGnd && std::fabs(V) > 0.0) {
        // First non-zero Vsrc tied to ground => treat as global VDD
        src.mType      = Vsrc::GLOBAL;
        seenGlobalVsrc = true;
    } else if (touchesPackage) {
        // Tie between package node(s) and something else
        src.mType = Vsrc::PACKAGE;
    }

    circ.mVsrcs.push_back(std::move(src));
}

void parseCurrentSource(const std::vector<std::string>& tokens,
                        CircuitGraph& circ, int32_t coordToMircoScale) {
    if (tokens.size() < 4) {
        PDN_FATAL("Current source line has fewer than 4 tokens: '%s'",
                  (tokens.empty() ? std::string{} : tokens[0]).c_str());
    }

    const std::string& name  = tokens[0];
    const std::string& n1Str = tokens[1];
    const std::string& n2Str = tokens[2];
    const std::string& val   = tokens[3];

    double I = parseSpiceNumber(val);

    ParsedNode pn1 = parseNodeName(n1Str, coordToMircoScale);
    ParsedNode pn2 = parseNodeName(n2Str, coordToMircoScale);

    circ.ensureNode(pn1.id, pn1.netIndex, pn1.x, pn1.y);
    circ.ensureNode(pn2.id, pn2.netIndex, pn2.x, pn2.y);

    Isrc src;
    src.mName     = IdString(name);
    src.mFromNode = pn1.id;
    src.mToNode   = pn2.id;
    src.mI        = I;

    std::string lowerName = toLower(name);
    if (startsWithIgnoreCase(lowerName, "ib")) {
        src.mType = Isrc::IB;
    } else {
        src.mType = Isrc::OTHER;
    }

    circ.mIsrcs.push_back(std::move(src));
}

} // anonymous namespace

namespace pdnsol {
// -------------------------
// Public API
// -------------------------

CircuitGraph parseSpice(std::istream& in, CircuitGraph::Unit coordUnit) {
    CircuitGraph circ;
    circ.mCoordinateUnit = coordUnit;

    const int32_t coordToMircoScale =
      (coordUnit == CircuitGraph::UM) ? 1 : 1000;

    ParseState st;
    bool       seenGlobalVsrc = false;

    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(in, line)) {
        ++lineNumber;
        std::string stripped = trim(line);
        if (stripped.empty())
            continue;

        // Comment or meta line
        if (stripped[0] == '*') {
            parseCommentMeta(stripped.substr(1), circ, st);
            continue;
        }

        // Dot commands (.op, .end, etc.) are ignored for now
        if (stripped[0] == '.') {
            continue;
        }

        // Line continuation (starting with '+') is not handled here.
        if (stripped[0] == '+') {
            // If your netlists use continuations, you can implement merging
            // here.
            throw std::runtime_error("Line continuation ('+') not supported "
                                     "in SPICE parser at line " +
                                     std::to_string(lineNumber));
        }

        auto tokens = splitWhitespace(stripped);
        if (tokens.empty())
            continue;

        char elemType = static_cast<char>(
          std::toupper(static_cast<unsigned char>(tokens[0][0])));

        try {
            switch (elemType) {
            case 'R':
                parseResistor(tokens, circ, st, coordToMircoScale);
                break;
            case 'V':
                parseVoltageSource(
                  tokens, circ, st, coordToMircoScale, seenGlobalVsrc);
                break;
            case 'I':
                parseCurrentSource(tokens, circ, coordToMircoScale);
                break;
            default:
                // Unknown / unsupported element; silently ignore and log
                PDN_ERROR("Unsupported element type at line %d: %s",
                          lineNumber,
                          line.c_str());
                break;
            }
        } catch (const std::exception& e) {
            PDN_FATAL("Error parsing line %d: %s: %s",
                      lineNumber,
                      line.c_str(),
                      e.what());
        }
    }

    // At this point circ contains nodes, metal/via/pkg resistors,
    // Vsrcs (including vias as Vsrc::VIA where appropriate), and Isrcs.
    return circ;
}

CircuitGraph parseSpiceFile(const std::string& path,
                            CircuitGraph::Unit coordUnit) {
    std::ifstream ifs(path);
    if (!ifs) {
        PDN_FATAL("Failed to open SPICE file: %s", path.c_str());
    }
    return parseSpice(ifs, coordUnit);
}
} // namespace pdnsol