#include <fstream>

#include "pdnsol/solver/mna.hpp"
#include "pdnsol/struct/circuit_decorator.hpp"
#include "pdnsol/utils/fixed_point_number.hpp"
#include "pdnsol/utils/logging.hpp"

namespace pdnsol {
// Decode netId into (layerIndex, isVdd).
// static NetDecomposition decodeNetId(int32_t netId) {
//     NetDecomposition d;
//     d.layer = netId / 2 + 1;
//     d.isVdd = ((netId % 2) == 1);
//     return d;
// }
// Helper: format unique current source name
static std::string formatIsrcName(IdString layer, IdString netName,
                                  std::size_t regionIdx,
                                  std::size_t localIdx) {
    std::ostringstream oss;
    oss << "I_" << layer.c_str() << "_" << netName.c_str() << "_r" << regionIdx
        << "_n" << localIdx;
    return oss.str();
}

CircuitDecorator::CircuitDecorator(CircuitGraph&          inGraph,
                                   const DecoratorConfig& cfg,
                                   const NetFilter&       netFilter)
    : mIn(inGraph)
    , mCfg(cfg)
    , mNetFilter(netFilter) {}

void CircuitDecorator::build() {
    addCurrentRegionsFromJson(mCfg.currentConfigPath, mIn);
    addVoltageSourceFromConfig(mCfg.voltageConfigPath, mIn);
}

// -------------------------------------------------------------------------
// Insertions
// -------------------------------------------------------------------------
void CircuitDecorator::addCurrentRegionsFromJson(
  const std::string& jsonFilePath, CircuitGraph& graph) {
    const auto regions = parseCurrentRegionsFromJson(jsonFilePath);

    for (std::size_t rIdx = 0; rIdx < regions.size(); ++rIdx) {
        const auto& region = regions[rIdx];

        for (const auto& [netName, I_total] : region.netCurrents) {
            addIsrcsForRegionNet(
              region.rect, region.layer, netName, I_total, graph, rIdx);
        }
    }
}

void CircuitDecorator::addVoltageSourceFromConfig(
  const std::string& configFilePath, CircuitGraph& graph) {
    // Landing layer for all voltage sources (user-specified)
    // Example: "M8", "TOP_METAL", etc
    const std::string& landingLayerStr = mCfg.voltageSourceLandingLayer;
    if (landingLayerStr.empty()) {
        PDN_FATAL("CircuitDecorator::addVoltageSourceFromConfig: "
                  "voltageSourceLandingLayer is empty in DecoratorConfig.");
    }
    // Convert landingLayer to IdString -- adapt to your IdString API
    const IdString landingLayerId(landingLayerStr);

    // Open configuration file
    std::ifstream ifs(configFilePath);
    if (!ifs) {
        PDN_FATAL("CircuitDecorator::addVoltageSourceFromConfig: cannot open "
                  "file '%s'",
                  configFilePath.c_str());
    }

    std::string line;
    std::size_t lineNo      = 0;
    // start after existing sources
    std::size_t vsrcCounter = graph.mVsrcs.size();

    auto isCommentOrEmpty = [](const std::string& line) {
        std::string t = trim(line);
        if (t.empty()) return true;
        if (t[0] == '#') return true;
        if (t.size() >= 2 && t[0] == '/' && t[1] == '/') return true;
        return false;
    };

    // Per-node aggregation info so that if multiple voltage sources map to the
    // same PDN node we can reduce the *equivalent* package resistance:
    // R_eq = R_user / N
    struct NodeVsrcInfo {
        std::size_t vsrcCount = 0; // how many Vsrcs ended up on this node
        std::size_t pkgResIdx = 0; // index into graph.mPkgResistors
        IdString    pkgNode;       // package-side node name of that resistor
    };

    // Keyed by *PDN node name* (IdString)
    IdString::Map<NodeVsrcInfo> nodeInfoMap;

    // Helper: iterate over graph.mNodes
    auto findClosestNodeOnLayerAndNet =
      [&graph, &landingLayerId](const IdString& netNameId,
                                Tick            xTick,
                                Tick            yTick,
                                IdString&       outNodeName) -> bool {
        bool        found  = false;
        long double bestD2 = 0.0L;
        // outNodeName will be populated on success

        for (const auto& kv : graph.mNodes) {
            const IdString& nodeName = kv.first;
            const Node&     node     = kv.second;

            if (node.net.get() < 0) {
                // Skip nodes with invalid net index
                continue;
            }

            // Map node.mNet (int32) -> NetKey
            NetId  nid(node.net);
            NetKey nk = graph.netKey(nid);

            // Filter by landing layer
            if (nk.layer != landingLayerId) continue;

            // Filter by net name
            if (nk.netName != netNameId) continue;

            // Compute squared distance in [um^2]
            const Tick        dx = node.x - xTick;
            const Tick        dy = node.y - yTick;
            const long double d2 =
              static_cast<long double>(dx) * static_cast<long double>(dx) +
              static_cast<long double>(dy) * static_cast<long double>(dy);

            if (!found || d2 < bestD2) {
                found       = true;
                bestD2      = d2;
                outNodeName = nodeName; // or node.name; they should match
            }
        }

        return found;
    };

    while (std::getline(ifs, line)) {
        ++lineNo;
        if (isCommentOrEmpty(line)) {
            continue;
        }

        std::istringstream iss(line);
        std::string        _; // bumpNameStr
        std::string        netNameStr;
        double             xCoord = 0.0;
        double             yCoord = 0.0;

        if (!(iss >> _ >> xCoord >> yCoord >> netNameStr)) {
            PDN_WARN("Warning: [addVoltageSourceFromConfig] Failed to "
                     "parse line %zu in '%s': '%s",
                     lineNo,
                     configFilePath.c_str(),
                     line.c_str());
            continue;
        }

        const IdString netNameId(netNameStr);

        // Name-only filter first (cheap, avoids noise)
        if (!mNetFilter.allowsName(netNameId)) {
            continue;
        }

        // If the graph doesn't even contain this net on the landing layer,
        // and the user is filtering, skip quietly unless explicitly requested
        const NetId landingNetId = graph.netId(landingLayerId, netNameId);
        if (!landingNetId) {
            if (mNetFilter.isAllowAll() ||
                mNetFilter.explicitlyIncludesName(netNameId)) {
                PDN_WARN("[addVoltageSourceFromConfig] Net '%s' not found on "
                         "landing layer '%s'. Skip this source.",
                         netNameStr.c_str(),
                         landingLayerStr.c_str());
            }
            continue;
        }

        const NetKey nk = graph.netKey(landingNetId);
        if (!mNetFilter.allows(nk.netName, nk.isPower, nk.isGround)) {
            continue;
        }

        auto vsrcProp = mCfg.voltageSources.find(netNameStr);
        if (vsrcProp == mCfg.voltageSources.end()) {
            PDN_WARN("Unknown voltage net %s. Skip", netNameStr.c_str());
            continue;
        }
        const ScalarType voltage  = vsrcProp->second.voltage;  // Volt
        const ScalarType packageR = vsrcProp->second.packageR; // Ohm

        // Convert coordinates to internal fixed-point number representation
        const Tick xTick = FPN::toRep(xCoord);
        const Tick yTick = FPN::toRep(yCoord);

        // 1) Find the closest PDN node on the landing layer for this net
        IdString   closestNodeName;
        const bool found = findClosestNodeOnLayerAndNet(
          netNameId, xTick, yTick, closestNodeName);

        if (!found) {
            PDN_WARN(
              "Warning: [addVoltageSourceFromConfig] No node found on "
              "layer '%s' for net '%s' near (%g, %g) in '%s' (line %zu). "
              "This voltage source is ignored.",
              landingLayerStr.c_str(),
              netNameStr.c_str(),
              xCoord,
              yCoord,
              configFilePath.c_str(),
              lineNo);
            continue;
        }

        // 2) Create / update package resistor for this node
        //
        //    We maintain one *logical* package resistor per PDN node, and if
        //    multiple voltage sources land on the same node, we shrink the
        //    effective resistance:
        //        R_eq = R_user / N
        //    where N is how many sources ended up on this PDN node
        //
        //    Topology used (conceptual):
        //
        //      (global ideal node) ── Vsrc ── (pkg node) ── R_pkg_eq ── (PDN
        //      node)
        //
        //    - (PDN node) is 'closestNodeName'
        //    - (pkg node) is a synthetic node name, unique per PDN node
        //      (e.g., "<pdnNode>_PKG")
        //    - (global ideal node) is a synthetic node name per net
        //      (e.g., "<net>_IDEAL")

        // (a) Lookup or create aggregation info for this PDN node
        auto itInfo = nodeInfoMap.find(closestNodeName);
        if (itInfo == nodeInfoMap.end()) {
            // First voltage source landing on this PDN node
            NodeVsrcInfo info;

            info.vsrcCount = 1;

            // Package-side node name for this PDN node
            // Feel free to change the naming scheme if you have conventions
            const std::string pkgNodeStr = closestNodeName.str() + "_PKG";
            info.pkgNode                 = IdString(pkgNodeStr);

            // Create a new package resistor: (pkgNode) --R_user--> (PDN node)
            PkgRes            pkg;
            const std::string pkgName =
              "RPKG_VSRC_" + std::to_string(vsrcCounter);
            pkg.name = IdString(pkgName);
            pkg.n1   = info.pkgNode;    // package-side node
            pkg.n2   = closestNodeName; // PDN node
            pkg.R    = packageR;        // first source -> R_eq = R_user

            graph.mPkgResistors.push_back(pkg);
            info.pkgResIdx = graph.mPkgResistors.size() - 1;

            itInfo = nodeInfoMap.emplace(closestNodeName, info).first;
        } else {
            // Another voltage source is attached to the *same* PDN node
            // Increase count and shrink equivalent R accordingly
            NodeVsrcInfo& info = itInfo->second;
            ++info.vsrcCount;

            const ScalarType newR = packageR / info.vsrcCount;

            graph.mPkgResistors[info.pkgResIdx].R = newR;
        }

        NodeVsrcInfo& info = itInfo->second;

        // 3) Create the voltage source itself
        //
        //    We connect:
        //      (global ideal node for <net>)  <->  (package-side node)
        //
        //    The actual voltage value (mV) is not specified by
        //    DecoratorConfig, so we leave it at 0.0 here. You can later sweep
        //    or set it according to your analysis needs
        //
        //    If your solver expects a different topology (for example a Vsrc
        //    directly between (PDN node) and some global node, with the
        //    package resistor handled elsewhere), modify mFromNode/mToNode
        //    wiring here accordingly

        Vsrc              vsrc;
        const std::string vsrcName = "VSRC_" + std::to_string(vsrcCounter);
        vsrc.name                  = IdString(vsrcName);
        // package-side node feeding R_pkg_eq
        vsrc.fromNode              = info.pkgNode;
        // Ideal reference ground
        vsrc.toNode                = IdString("GND");
        vsrc.type                  = Vsrc::PACKAGE;
        vsrc.V                     = voltage;

        graph.ensureNode(info.pkgNode);
        graph.mVsrcs.push_back(vsrc);
        ++vsrcCounter;
    } // while getline
}

// Main function for one region + one net
void CircuitDecorator::addIsrcsForRegionNet(const RectRegion& rect,
                                            IdString layer, IdString netName,
                                            double        totalCurrent,
                                            CircuitGraph& graph,
                                            std::size_t   regionIdx) {
    // Name-only filter first (fast, avoids noisy "missing net" warnings)
    if (!mNetFilter.allowsName(netName)) return;

    // 1) Resolve mNet id
    const NetId netId = graph.netId(layer, netName);
    if (!netId) {
        if (mNetFilter.isAllowAll() ||
            mNetFilter.explicitlyIncludesName(netName)) {
            PDN_WARN("No netId for layer=%s, net=%s (skipping current region)",
                     layer.c_str(),
                     netName.c_str());
        }
        return;
    }
    const NetKey netKey = graph.netKey(netId);

    // Full filter with type info
    if (!mNetFilter.allows(netKey.netName, netKey.isPower, netKey.isGround)) {
        return;
    }

    // 2) Collect candidate nodes in that region and net
    struct NodeRef {
        const Node* node;
    };

    std::vector<NodeRef> candidates;
    candidates.reserve(128);

    auto contains = [](const RectRegion& rect, Tick x, Tick y) {
        // inclusive bounds
        return (x >= rect.xMin && x <= rect.xMax && y >= rect.yMin &&
                y <= rect.yMax);
    };
    for (const auto& kv : graph.mNodes) {
        const Node& n = kv.second;
        if (n.net != netId) continue;
        if (!contains(rect, n.x, n.y)) continue;

        candidates.push_back(NodeRef{&n});
    }

    if (candidates.empty()) {
        PDN_WARN("No nodes found in region (layer=%s , net=%s) for specified "
                 "rectangle.",
                 layer.c_str(),
                 netName.c_str());
        return;
    }

    // 4) Determine which nodes get what current
    std::vector<double> nodeCurrents(candidates.size(), 0.0);
    const double        I_per_node = totalCurrent / candidates.size();
    for (auto& I : nodeCurrents) {
        I = I_per_node;
    }

    // 5) Create Isrc entries in graph.mIsrcs
    const bool isPower = netKey.isPower;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const double I = nodeCurrents[i];
        if (std::abs(I) <= 0.0) continue; // skip zero

        const Node* n = candidates[i].node;

        Isrc is;
        is.name     = IdString(formatIsrcName(layer, netName, regionIdx, i));
        is.fromNode = isPower ? n->name : IdString("GND");
        is.toNode   = isPower ? IdString("GND") : n->name;
        is.type     = Isrc::IB;
        is.I        = static_cast<ScalarType>(I);

        graph.mIsrcs.push_back(std::move(is));
    }
}

// -------------------------------------------------------------------------
// Parsers
// -------------------------------------------------------------------------
std::vector<CircuitDecorator::CurrentRegion>
CircuitDecorator::parseCurrentRegionsFromJson(
  const std::string& jsonFilePath) {
    std::ifstream ifs(jsonFilePath);
    if (!ifs) {
        PDN_FATAL("Cannot open current-region JSON file: %s",
                  jsonFilePath.c_str());
    }

    Json j = Json::parse(ifs);

    if (!j.contains("current_regions") || !j["current_regions"].is_array()) {
        PDN_FATAL("JSON %s missing 'current_regions' array.",
                  jsonFilePath.c_str());
    }

    std::vector<CurrentRegion> regions;
    for (const auto& regionJ : j["current_regions"]) {
        CurrentRegion r;

        // ---- area ----
        if (!regionJ.contains("area") || !regionJ["area"].is_array() ||
            regionJ["area"].size() != 4) {
            throw std::runtime_error(
              "Each current_region must have 'area' of size 4.");
        }

        double xMinUm = regionJ["area"][0].get<double>();
        double yMinUm = regionJ["area"][1].get<double>();
        double xMaxUm = regionJ["area"][2].get<double>();
        double yMaxUm = regionJ["area"][3].get<double>();

        r.rect.xMin = FPN::toRep(xMinUm);
        r.rect.yMin = FPN::toRep(yMinUm);
        r.rect.xMax = FPN::toRep(xMaxUm);
        r.rect.yMax = FPN::toRep(yMaxUm);

        // ---- layer ----
        if (!regionJ.contains("layer") || !regionJ["layer"].is_string()) {
            PDN_FATAL("Each current_region must have a 'layer' string.");
        }
        r.layer = IdString(regionJ["layer"].get<std::string>());

        // ---- current map ----
        if (!regionJ.contains("current") || !regionJ["current"].is_object()) {
            PDN_FATAL("Each current_region must have 'current' object.");
        }

        for (auto it = regionJ["current"].begin();
             it != regionJ["current"].end();
             ++it) {
            const IdString netName = IdString::tryLookup(it.key());
            const double   I       = it.value().get<double>();
            // skip zero currents silently
            if (!netName.valid() || std::abs(I) <= 0.0) {
                continue;
            }
            r.netCurrents.emplace(netName, I);
        }

        regions.push_back(std::move(r));
    }

    return regions;
}
} // namespace pdnsol
