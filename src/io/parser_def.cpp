#include "pdnsol/io/parser_def.hpp"

#include <cctype>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "pdnsol/common.hpp"
#include "pdnsol/io/parser_utils.hpp" // trim, splitWhitespace, iequals, toLower
#include "pdnsol/utils/logging.hpp"

namespace {
struct DefPin;
struct DefComponent;

using namespace pdnsol;
using PinMap = std::unordered_map<IdString, DefPin, IdString::Hash>;
using ComponentMap =
  std::unordered_map<IdString, DefComponent, IdString::Hash>;
// -------------------------
// DEF data structures
// -------------------------

struct DefPin {
    IdString name; // Logical pin name (e.g., "VDD")
    IdString net;  // Connected net name (e.g., "VDD")
    ScalarType xMeters = 0.0;
    ScalarType yMeters = 0.0;
};

struct DefComponent {
    IdString name;  // Instance name (e.g., "U1")
    IdString macro; // Master cell/macroname (e.g., "NAND2_X1")
    ScalarType xMeters = 0.0;
    ScalarType yMeters = 0.0;
};

struct DefDesign {
    double dbuPerMicron = 1.0; // From UNITS DISTANCE MICRONS <dbuPerMicron>
    double dbuToMeters = 1e-6; // computed = 1e-6 / dbuPerMicron

    PinMap pins;        // pinName -> DefPin
    ComponentMap comps; // instName -> DefComponent
};

enum class DefSection { NONE, UNITS, COMPONENTS, PINS, SPECIALNETS, NETS };

// Helper: parse "( x y )" style coordinate from tokens.
bool extractXYFromTokens(const std::vector<std::string>& tokens, int& xDbu,
                         int& yDbu) {
    for (std::size_t i = 0; i + 3 < tokens.size(); ++i) {
        if (tokens[i] == "(") {
            try {
                xDbu = std::stoi(tokens[i + 1]);
                yDbu = std::stoi(tokens[i + 2]);
                return true;
            } catch (...) { return false; }
        }
    }
    return false;
}

// Main DEF parsing: only UNITS / COMPONENTS / PINS for now.
DefDesign parseDef(std::istream& in) {
    DefDesign def;

    DefSection section = DefSection::NONE;

    std::string line;
    std::size_t lineNumber = 0;

    // State for PINS
    bool inPin = false;
    DefPin curPin;

    // State for COMPONENTS
    bool inComp = false;
    DefComponent curComp;

    while (std::getline(in, line)) {
        ++lineNumber;
        std::string stripped = trim(line);
        if (stripped.empty()) continue;
        if (stripped[0] == '#') continue; // comment

        auto tokens = splitWhitespace(stripped);
        if (tokens.empty()) continue;

        // Section starts / ends
        if (iequals(tokens[0], "UNITS")) {
            section = DefSection::UNITS;
            continue;
        }
        if (iequals(tokens[0], "COMPONENTS")) {
            section = DefSection::COMPONENTS;
            inComp = false;
            continue;
        }
        if (iequals(tokens[0], "PINS")) {
            section = DefSection::PINS;
            inPin = false;
            continue;
        }
        if (iequals(tokens[0], "SPECIALNETS")) {
            section = DefSection::SPECIALNETS;
            // You can extend here later for Option B with routing.
            continue;
        }
        if (iequals(tokens[0], "NETS")) {
            section = DefSection::NETS;
            // You can extend if you want signal net routing as well.
            continue;
        }
        if (iequals(tokens[0], "END")) {
            if (tokens.size() >= 2) {
                if (iequals(tokens[1], "UNITS") &&
                    section == DefSection::UNITS) {
                    section = DefSection::NONE;
                } else if (iequals(tokens[1], "COMPONENTS") &&
                           section == DefSection::COMPONENTS) {
                    section = DefSection::NONE;
                } else if (iequals(tokens[1], "PINS") &&
                           section == DefSection::PINS) {
                    section = DefSection::NONE;
                } else if (iequals(tokens[1], "SPECIALNETS") &&
                           section == DefSection::SPECIALNETS) {
                    section = DefSection::NONE;
                } else if (iequals(tokens[1], "NETS") &&
                           section == DefSection::NETS) {
                    section = DefSection::NONE;
                }
            }
            continue;
        }

        // -------------------------
        // UNITS section
        // -------------------------
        if (section == DefSection::UNITS) {
            // Expect: DISTANCE MICRONS <dbuPerMicron> ;
            if (tokens.size() >= 4 && iequals(tokens[0], "DISTANCE") &&
                iequals(tokens[1], "MICRONS")) {
                try {
                    def.dbuPerMicron = std::stod(tokens[2]);
                    if (def.dbuPerMicron <= 0) {
                        throw std::runtime_error(
                          "Invalid dbuPerMicron in DEF UNITS");
                    }
                    def.dbuToMeters = 1e-6 / def.dbuPerMicron;
                } catch (const std::exception& e) {
                    throw std::runtime_error(
                      "Failed to parse UNITS DISTANCE MICRONS at line " +
                      std::to_string(lineNumber) + ": " + e.what());
                }
            }
            continue;
        }

        // -------------------------
        // COMPONENTS section
        // -------------------------
        if (section == DefSection::COMPONENTS) {
            // Start of component: "- instName macroName ..."
            if (tokens[0] == "-") {
                if (inComp) {
                    // flush previous incomplete component
                    if (curComp.name.valid()) {
                        def.comps[curComp.name] = curComp;
                    }
                }
                if (tokens.size() < 3) {
                    throw std::runtime_error("Malformed COMPONENT at line " +
                                             std::to_string(lineNumber));
                }
                inComp = true;
                curComp = DefComponent{};
                curComp.name = IdString(tokens[1]);
                curComp.macro = IdString(tokens[2]);
                continue;
            }

            if (!inComp) {
                // Some line in COMPONENTS, but not inside a component
                continue;
            }

            // Look for "+ PLACED" or "+ FIXED"
            if (tokens.size() >= 2 && tokens[0] == "+") {
                if (iequals(tokens[1], "PLACED") ||
                    iequals(tokens[1], "FIXED")) {
                    int xDbu = 0, yDbu = 0;
                    if (extractXYFromTokens(tokens, xDbu, yDbu)) {
                        PDN_FATAL_IF(xDbu * def.dbuToMeters < 0.0,
                                     "Coordinate-x cannot be negative");
                        PDN_FATAL_IF(yDbu * def.dbuToMeters < 0.0,
                                     "Coordinate-y cannot be negative");
                        curComp.xMeters = xDbu * def.dbuToMeters;
                        curComp.yMeters = yDbu * def.dbuToMeters;
                    }
                }
            }

            // Component terminator ";"
            if (!tokens.empty() &&
                tokens.back().find(';') != std::string::npos) {
                if (curComp.name.valid()) {
                    def.comps[curComp.name] = curComp;
                }
                inComp = false;
            }

            continue;
        }

        // -------------------------
        // PINS section
        // -------------------------
        if (section == DefSection::PINS) {
            // Start of pin: "- pinName"
            if (tokens[0] == "-") {
                if (inPin) {
                    if (curPin.name.valid()) {
                        def.pins[curPin.name] = curPin;
                    }
                }
                if (tokens.size() < 2) {
                    throw std::runtime_error("Malformed PIN at line " +
                                             std::to_string(lineNumber));
                }
                inPin = true;
                curPin = DefPin{};
                curPin.name = IdString(tokens[1]);
                continue;
            }

            if (!inPin) { continue; }

            // Handle "+ NET <netName>" etc.
            if (tokens.size() >= 3 && tokens[0] == "+" &&
                iequals(tokens[1], "NET")) {
                curPin.net = IdString(tokens[2]);
            }

            // "+ PLACED ( x y ) ORIENT ;" or "+ FIXED ..."
            if (tokens.size() >= 2 && tokens[0] == "+") {
                if (iequals(tokens[1], "PLACED") ||
                    iequals(tokens[1], "FIXED")) {
                    int xDbu = 0, yDbu = 0;
                    if (extractXYFromTokens(tokens, xDbu, yDbu)) {
                        PDN_FATAL_IF(xDbu * def.dbuToMeters < 0.0,
                                     "Coordinate-x cannot be negative");
                        PDN_FATAL_IF(yDbu * def.dbuToMeters < 0.0,
                                     "Coordinate-y cannot be negative");
                        curPin.xMeters = xDbu * def.dbuToMeters;
                        curPin.yMeters = yDbu * def.dbuToMeters;
                    }
                }
            }

            // PIN terminator ";"
            if (!tokens.empty() &&
                tokens.back().find(';') != std::string::npos) {
                if (curPin.name.valid()) { def.pins[curPin.name] = curPin; }
                inPin = false;
            }

            continue;
        }

        // -------------------------
        // SPECIALNETS / NETS
        // -------------------------
        if (section == DefSection::SPECIALNETS ||
            section == DefSection::NETS) {
            // Parsing of ROUTED geometry for Option B can be added here.
            // For now we ignore them in this base implementation.
            continue;
        }

        // Other sections ignored.
    }

    // Flush any dangling component/pin if needed
    if (inComp && curComp.name.valid()) { def.comps[curComp.name] = curComp; }
    if (inPin && curPin.name.valid()) { def.pins[curPin.name] = curPin; }

    return def;
}

// -------------------------
// Augment CircuitGraph: anchors
// -------------------------

// Assign coordinates to CircuitGraph nodes using DEF pins & components.
void applyAnchorsFromDef(const DefDesign& def, CircuitGraph& circ) {
    for (auto& kv : circ.mNodes) {
        IdString name = kv.first;
        Node& node = kv.second;

        // 1) Exact pin match: node named exactly like a DEF pin
        auto pit = def.pins.find(name);
        if (pit != def.pins.end() && pit->second.xMeters >= 0.0 &&
            pit->second.yMeters >= 0.0) {
            node.mX = pit->second.xMeters;
            node.mY = pit->second.yMeters;
            continue;
        }

        // 2) Instance pin: "inst/pin" or "inst:pin" → place at instance origin
        // (approx)
        std::size_t slashPos = name.str().find('/');
        if (slashPos == std::string::npos) {
            slashPos = name.str().find(':'); // some tools use inst:pin
        }

        if (slashPos != std::string::npos && slashPos > 0) {
            IdString instName =
              IdString::tryLookup(name.str().substr(0, slashPos));
            if (!instName.valid()) { continue; }
            auto cit = def.comps.find(instName);
            if (cit != def.comps.end() && cit->second.xMeters >= 0.0 &&
                cit->second.yMeters >= 0.0) {
                node.mX = cit->second.xMeters;
                node.mY = cit->second.yMeters;
                continue;
            }
        }

        // 3) May add more heuristics here:
        //    - Name-map variants: "*12" => actual name
        //    - Pin arrays: "VDD[0]" vs "VDD"
        //    - Net-based anchors, etc.
    }
}

// -------------------------
// Approximate positions for internal nodes
// -------------------------

// Build adjacency from resistors (metal and via).
std::unordered_map<IdString, std::vector<IdString>, IdString::Hash>
buildAdjacency(const CircuitGraph& circ) {
    std::unordered_map<IdString, std::vector<IdString>, IdString::Hash> adj;

    auto addEdge = [&adj](const IdString& a, const IdString& b) {
        adj[a].push_back(b);
        adj[b].push_back(a);
    };

    for (const auto& r : circ.mMetalResistors) {
        addEdge(r.mN1, r.mN2);
    }
    for (const auto& r : circ.mViaResistors) {
        addEdge(r.mN1, r.mN2);
    }
    for (const auto& r : circ.mPkgResistors) {
        addEdge(r.mN1, r.mN2);
    }
    return adj;
}

// Simple heuristic: iteratively assign unknown-node positions to
// the average of any neighbors that already have coordinates.
//
// This does NOT use explicit DEF routing (no polylines / layers).
// It uses only the electrical topology and the anchors you got from DEF.
// It is a "graph smoothing" / barycentric interpolation, which is
// relatively cheap and often good enough for visualization.
//
// For a more accurate approximation, I shall replace this by a routine
// that walks over DEF ROUTED polylines and distributes intermediate
// nodes along those paths according to resistance ratios.
void propagateApproxInternalPositions(CircuitGraph& circ) {
    auto adj = buildAdjacency(circ);

    // Track which nodes have "known" coordinates.
    std::unordered_map<IdString, bool, IdString::Hash> hasCoord;
    hasCoord.reserve(circ.mNodes.size());

    auto isValid = [](Tick v) { return v >= 0; };

    for (const auto& kv : circ.mNodes) {
        const IdString& id = kv.first;
        const Node& node = kv.second;
        bool known = isValid(node.mX) && isValid(node.mY);
        hasCoord[id] = known;
    }

    bool changed = true;
    const int maxIter = 50; // keep it modest
    int iter = 0;

    while (changed && iter < maxIter) {
        changed = false;
        ++iter;

        for (auto& kv : circ.mNodes) {
            const IdString& id = kv.first;
            Node& node = kv.second;

            if (hasCoord[id]) continue; // already known

            auto itAdj = adj.find(id);
            if (itAdj == adj.end()) continue;

            Tick sumX = 0.0;
            Tick sumY = 0.0;
            int count = 0;

            for (const auto& nbId : itAdj->second) {
                auto itHC = hasCoord.find(nbId);
                if (itHC != hasCoord.end() && itHC->second) {
                    const Node& nbNode = circ.mNodes.at(nbId);
                    if (isValid(nbNode.mX) && isValid(nbNode.mY)) {
                        sumX += nbNode.mX;
                        sumY += nbNode.mY;
                        ++count;
                    }
                }
            }

            if (count > 0) {
                node.mX = std::llround((double)sumX / count);
                node.mY = std::llround((double)sumY / count);
                hasCoord[id] = true;
                changed = true;
            }
        }
    }

    // Any remaining nodes without coordinates after maxIter
    // will remain at whatever default they had (e.g., 0,0).
}

} // anonymous namespace

namespace pdnsol {

// -------------------------
// Public API
// -------------------------

void augmentCircuitGraphWithDef(std::istream& defIn, CircuitGraph& circ,
                                bool propagateInternalNodes) {
    DefDesign def = parseDef(defIn);

    // 1) Set coordinates for nodes that can be directly anchored
    //    by PINS or COMPONENTS.
    applyAnchorsFromDef(def, circ);

    // 2) Optionally propagate positions to internal nodes
    //    using graph smoothing.
    if (propagateInternalNodes) { propagateApproxInternalPositions(circ); }
}

void augmentCircuitGraphWithDefFile(const std::string& path,
                                    CircuitGraph& circ,
                                    bool propagateInternalNodes) {
    std::ifstream ifs(path);
    if (!ifs) { throw std::runtime_error("Failed to open DEF file: " + path); }
    augmentCircuitGraphWithDef(ifs, circ, propagateInternalNodes);
}

} // namespace pdnsol