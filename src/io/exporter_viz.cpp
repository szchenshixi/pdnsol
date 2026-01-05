#include "pdnsol/io/exporter_viz.hpp"

#include <filesystem>
#include <fstream>

#include "pdnsol/common.hpp"
#include "pdnsol/utils/fixed_point_number.hpp"
#include "pdnsol/utils/logging.hpp"

namespace pdnsol {
static inline std::string layerOfNode(const CircuitGraph& c,
                                      const IdString&     nid) {
    auto it = c.mNodes.find(nid);
    if (it == c.mNodes.end()) return "UNKNOWN_NODE";
    const Node& n = it->second;
    if (!n.mNet) return "UNASSIGNED_NET";
    auto k = c.netKey(n.mNet);
    return k.layer.str();
}

void exportCircuitGraphForVizJson(const CircuitGraph& c,
                                  const std::string&  outPath) {
    Json j;

    // ---- Nodes ----
    j["nodes"] = Json::array();
    j["nodes"].get<Json::array_t*>()->reserve(c.mNodes.size());

    for (const auto& [nodeId, node] : c.mNodes) {
        std::string layer   = "UNASSIGNED_NET";
        std::string net     = "UNASSIGNED_NET";
        bool        isPower = false, isGround = false;
        int         netId = -1;

        if (node.mNet) {
            auto k   = c.netKey(node.mNet);
            layer    = k.layer.c_str();
            net      = k.netName.c_str();
            isPower  = k.isPower;
            isGround = k.isGround;
            netId    = node.mNet.get();
        }

        j["nodes"].push_back({{"id", nodeId.str()},
                              {"x_um", FPN::fromRep(node.mX)},
                              {"y_um", FPN::fromRep(node.mY)},
                              {"net_id", netId},
                              {"layer", layer},
                              {"net", net},
                              {"is_power", isPower},
                              {"is_ground", isGround}});
    }

    // ---- Edges ----
    j["edges"] = Json::array();
    j["edges"].get_ptr<Json::array_t*>()->reserve(
      c.mMetalResistors.size() + c.mViaResistors.size() +
      c.mPkgResistors.size() + c.mVsrcs.size() + c.mIsrcs.size());

    // Metal resistors (layer known from res.mNet)
    for (const auto& r : c.mMetalResistors) {
        std::string layer = "UNASSIGNED_NET";
        if (r.mNet) layer = c.netKey(r.mNet).layer.c_str();

        j["edges"].push_back({{"id", r.mName.c_str()},
                              {"type", "metal"},
                              {"layer", layer},
                              {"n1", r.mN1.c_str()},
                              {"n2", r.mN2.c_str()},
                              {"r", r.mR}});
    }

    // Vias (layer inferred from node nets)
    for (const auto& v : c.mViaResistors) {
        j["edges"].push_back({{"id", v.mName.c_str()},
                              {"type", "via"},
                              {"n1", v.mN1.c_str()},
                              {"n2", v.mN2.c_str()},
                              {"r", v.mR}});
    }

    // Tsvs (layer inferred from node nets)
    for (const auto& v : c.mTsvResistors) {
        j["edges"].push_back({{"id", v.mName.c_str()},
                              {"type", "tsv"},
                              {"n1", v.mN1.c_str()},
                              {"n2", v.mN2.c_str()},
                              {"r", v.mR}});
    }

    // Package resistors
    for (const auto& p : c.mPkgResistors) {
        j["edges"].push_back({{"id", p.mName.c_str()},
                              {"type", "pkg"},
                              {"n1", p.mN1.c_str()},
                              {"n2", p.mN2.c_str()},
                              {"r", p.mR}});
    }

    // Voltage sources
    for (const auto& s : c.mVsrcs) {
        const char* subtype = (s.mType == Vsrc::GLOBAL)    ? "global"
                              : (s.mType == Vsrc::VIA)     ? "via"
                              : (s.mType == Vsrc::PACKAGE) ? "package"
                                                           : "other";

        j["edges"].push_back({{"id", s.mName.c_str()},
                              {"type", "vsrc"},
                              {"subtype", subtype},
                              {"n1", s.mFromNode.c_str()},
                              {"n2", s.mToNode.c_str()},
                              {"v", s.mV}});
    }

    // Current sources
    for (const auto& s : c.mIsrcs) {
        const char* subtype = (s.mType == Isrc::IB) ? "iB" : "other";

        j["edges"].push_back({{"id", s.mName.c_str()},
                              {"type", "isrc"},
                              {"subtype", subtype},
                              {"n1", s.mFromNode.c_str()},
                              {"n2", s.mToNode.c_str()},
                              {"i", s.mI}});
    }

    std::filesystem::path out(outPath);
    std::filesystem::path outDir = out.parent_path();
    if (!std::filesystem::exists(outDir)) {
        if (std::filesystem::create_directories(outDir)) {
            PDN_INFO("Created directory %s", outDir.c_str());
        } else {
            PDN_ERROR("Failed to create directory %s", outDir.c_str());
        }
    }

    std::ofstream os(outPath);
    os << j.dump(2);
    os.close();
}
} // namespace pdnsol