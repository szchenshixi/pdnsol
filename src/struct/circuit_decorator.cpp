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
                                   const DecoratorConfig& cfg)
    : mIn(inGraph)
    , mCfg(cfg) {
}

void CircuitDecorator::build() {
    addCurrentRegionsFromJson(mCfg.currentConfigPath, mIn);
}
// -------------------------------------------------------------------------
// High-Level Entries
// -------------------------------------------------------------------------
void CircuitDecorator::attachCurrents(CircuitGraph&      graph,
                                      const std::string& jsonFilePath) {
    // 1. add current sources
    addCurrentRegionsFromJson(jsonFilePath, graph);
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
// Main function for one region + one net
void CircuitDecorator::addIsrcsForRegionNet(const RectRegion& rect,
                                            IdString layer, IdString netName,
                                            double        totalCurrent,
                                            CircuitGraph& graph,
                                            std::size_t   regionIdx) {
    // 1) Resolve mNet id
    const NetId& netId = graph.netId(layer, netName);
    if (!netId) {
        PDN_WARNING("No netId for layer=%s, net=%s (skipping current region)",
                    layer.c_str(),
                    netName.c_str());
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
        if (n.mNet != netId) continue;
        if (!contains(rect, n.mX, n.mY)) continue;

        candidates.push_back(NodeRef{&n});
    }

    if (candidates.empty()) {
        PDN_WARNING(
          "No nodes found in region (layer=%s , net=%s) for specified "
          "rectangle.",
          layer,
          netName);
        return;
    }

    // 4) Determine which nodes get what current
    std::vector<double> nodeCurrents(candidates.size(), 0.0);
    const double        I_per_node =
      totalCurrent / static_cast<double>(candidates.size());
    for (auto& I : nodeCurrents) {
        I = I_per_node;
    }

    // 5) Create Isrc entries in graph.mIsrcs
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const double I = nodeCurrents[i];
        if (std::abs(I) == 0.0) continue; // skip zero

        const Node* n = candidates[i].node;

        Isrc is;
        is.mName     = IdString(formatIsrcName(layer, netName, regionIdx, i));
        is.mFromNode = n->mName; // Node holds its own IdString
        is.mToNode   = IdString("GND");
        is.mType     = Isrc::IB;
        is.mI        = static_cast<ScalarType>(I);

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

        double xMinUm   = regionJ["area"][0].get<double>();
        double yMinUm   = regionJ["area"][1].get<double>();
        double widthUm  = regionJ["area"][2].get<double>();
        double heightUm = regionJ["area"][3].get<double>();

        r.rect.xMin = FPN::toRep(xMinUm);
        r.rect.yMin = FPN::toRep(yMinUm);
        r.rect.xMax = r.rect.xMin + FPN::toRep(widthUm);
        r.rect.yMax = r.rect.yMin + FPN::toRep(heightUm);

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