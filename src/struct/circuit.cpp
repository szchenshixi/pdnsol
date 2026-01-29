#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/fixed_point_number.hpp"
#include "pdnsol/utils/id_string.hpp"
#include "pdnsol/utils/logging.hpp"

#include <cmath>
#include <stdexcept>

namespace pdnsol {
namespace {

struct NodePair {
    IdString a;
    IdString b;

    NodePair() = default;
    NodePair(IdString n1, IdString n2) {
        if (n1 < n2) {
            a = n1;
            b = n2;
        } else {
            a = n2;
            b = n1;
        }
    }

    bool operator==(const NodePair& other) const noexcept {
        return a == other.a && b == other.b;
    }
};

struct NodePairHash {
    std::size_t operator()(const NodePair& p) const noexcept {
        std::size_t h1 = IdString::Hash{}(p.a);
        std::size_t h2 = IdString::Hash{}(p.b);
        // Simple but decent hash combine
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

template <typename ResT>
void mergeParallelResistors(std::vector<ResT>& resVec) {
    struct Agg {
        double Gsum    = 0.0; // sum of conductances
        bool   hasZero = false;
        ResT   exemplar{}; // to preserve some metadata if desired
        bool   hasExemplar = false;
    };

    std::unordered_map<NodePair, Agg, NodePairHash> table;
    table.reserve(resVec.size());

    for (const auto& r : resVec) {
        if (r.n1 == r.n2) {
            // Resistor from node to itself is useless in DC; drop it
            continue;
        }
        NodePair key(r.n1, r.n2);
        Agg&     agg = table[key];

        if (!agg.hasExemplar) {
            agg.exemplar    = r;
            agg.hasExemplar = true;
        }

        if (r.R == 0.0) {
            // A 0-ohm short dominates anything in parallel
            agg.hasZero = true;
            agg.Gsum    = 0.0; // ignore other conductances
        } else if (!agg.hasZero) {
            agg.Gsum += 1.0 / r.R;
        }
    }

    resVec.clear();
    resVec.reserve(table.size());

    for (auto& kv : table) {
        const NodePair& key = kv.first;
        Agg&            agg = kv.second;

        ResT merged = agg.exemplar; // start from exemplar to keep metadata
        merged.n1   = key.a;
        merged.n2   = key.b;

        if (agg.hasZero) {
            merged.R = 0.0;
        } else if (agg.Gsum > 0.0) {
            merged.R = 1.0 / agg.Gsum;
        } else {
            // No valid conductance accumulated: should only happen
            // if everything was dropped; skip in that case
            continue;
        }

        resVec.push_back(std::move(merged));
    }
}

void mergeParallelIsrcs(std::vector<Isrc>& isrcs, double absTol = 1e-18) {
    struct Agg {
        double Isigned =
          0.0; // positive: a->b, negative: b->a, where (a,b) = NodePair
        Isrc exemplar{};
        bool hasExemplar = false;
    };

    std::unordered_map<NodePair, Agg, NodePairHash> table;
    table.reserve(isrcs.size());

    for (const auto& s : isrcs) {
        if (s.fromNode == s.toNode) {
            continue; // no effect
        }

        NodePair key(s.fromNode, s.toNode);
        Agg&     agg = table[key];

        if (!agg.hasExemplar) {
            agg.exemplar    = s;
            agg.hasExemplar = true;
        }

        // Determine orientation relative to canonical pair
        double sign = (key.a == s.fromNode && key.b == s.toNode) ? 1.0 : -1.0;
        agg.Isigned += sign * s.I;
    }

    isrcs.clear();

    for (auto& kv : table) {
        const NodePair& key = kv.first;
        Agg&            agg = kv.second;

        if (std::fabs(agg.Isigned) < absTol) {
            // Net current cancels out
            continue;
        }

        Isrc merged = agg.exemplar; // keep metadata
        if (agg.Isigned >= 0.0) {
            merged.fromNode = key.a;
            merged.toNode   = key.b;
            merged.I        = agg.Isigned;
        } else {
            merged.fromNode = key.b;
            merged.toNode   = key.a;
            merged.I        = -agg.Isigned;
        }

        isrcs.push_back(std::move(merged));
    }
}

void dedupVsrcs(std::vector<Vsrc>& vsrcs, double eps = 1e-9) {
    struct Group {
        bool   haveCanonical = false;
        double Vcanon        = 0.0; // voltage from a->b (where (a,b)=NodePair)
        Vsrc   representative{};
        // We also store conflicting ones separately
        std::vector<Vsrc> conflicts;
    };

    std::unordered_map<NodePair, Group, NodePairHash> table;
    table.reserve(vsrcs.size());

    for (const auto& s : vsrcs) {
        if (s.fromNode == s.toNode) {
            continue; // no effect in DC
        }

        NodePair key(s.fromNode, s.toNode);
        Group&   g = table[key];

        // Voltage w.R.t canonical orientation
        double Vcanon =
          (key.a == s.fromNode && key.b == s.toNode) ? s.V : -s.V;

        if (!g.haveCanonical) {
            g.haveCanonical  = true;
            g.Vcanon         = Vcanon;
            g.representative = s;
        } else {
            if (std::fabs(Vcanon - g.Vcanon) <= eps) {
                // Same constraint, redundant; drop it
            } else {
                // Different voltage between same nodes: keep as conflict
                g.conflicts.push_back(s);
            }
        }
    }

    vsrcs.clear();
    vsrcs.reserve(table.size()); // plus a few conflicts

    for (auto& kv : table) {
        Group& g = kv.second;
        if (!g.haveCanonical) continue;

        // Keep one representative
        vsrcs.push_back(std::move(g.representative));

        if (g.conflicts.empty()) {
            continue;
        }
        PDN_WARN("Conflicting vsrc representative from %s to %s %.2fV",
                 g.representative.fromNode.c_str(),
                 g.representative.toNode.c_str(),
                 g.representative.V);
        // And all conflicting ones, if any
        for (auto& c : g.conflicts) {
            vsrcs.push_back(std::move(c));
            PDN_WARN("Conflicting vsrc source from %s to %s %.2fV",
                     c.fromNode.c_str(),
                     c.toNode.c_str(),
                     c.V);
        }
    }
}
} // anonymous namespace

NetId CircuitGraph::netId(IdString layer, IdString name) const {
    if (!layer.valid() || !name.valid()) return NetId::Invalid;
    NetKey key{layer, name};
    auto   it = mNet2Id.find(key);
    if (it == mNet2Id.end()) return NetId::Invalid;
    return it->second;
}
NetKey CircuitGraph::netKey(NetId netId) const {
    if (netId.get() < 0 || netId.get() >= mId2Net.size()) {
        return NetKey{};
    }
    return mId2Net[netId.get()];
}
NetId CircuitGraph::registerNet(IdString layer, IdString name, bool isPwr,
                                bool isGnd) {
    if ((!isPwr && !isGnd) || (isPwr && isGnd)) {
        PDN_WARN("Found a net that is neither power nor ground: %s",
                 name.c_str());
    }
    NetKey  key{layer, name, isPwr, isGnd};
    int32_t nextId      = static_cast<int32_t>(mId2Net.size());
    auto [it, inserted] = mNet2Id.emplace(key, nextId);
    if (!inserted) {
        // already exists
        return it->second;
    }
    mId2Net.push_back(key);
    PDN_FATAL_IF(mId2Net.size() != mNet2Id.size(), "Inconsistent net map");
    return NetId{static_cast<int32_t>(mId2Net.size()) - 1};
}

Node& CircuitGraph::ensureNode(const IdString& name, int net,
                               std::optional<double> x,
                               std::optional<double> y) {
    if (name == "GND") {
        auto it = mNodes.find(name);
        if (it == mNodes.end()) {
            Node groundNode;
            groundNode.name = GND;
            it = mNodes.emplace(name, std::move(groundNode)).first;
        }
        return it->second;
    }

    auto it = mNodes.find(name);
    if (it != mNodes.end()) {
        Node& node = it->second;
        if (net >= 0 && !node.net) node.net = NetId(net);
        if (x && node.x < 0) node.x = FPN::toRep(*x);
        if (y && node.y < 0) node.y = FPN::toRep(*y);
        return node;
    }

    Node newNode;
    newNode.name       = name;
    newNode.net        = NetId(net);
    newNode.x          = x ? FPN::toRep(*x) : -1;
    newNode.y          = y ? FPN::toRep(*y) : -1;
    auto [insertIt, _] = mNodes.emplace(name, std::move(newNode));
    return insertIt->second;
}

void CircuitGraph::ensureAllReferencedNodesExist() {
    IdString::Set names;
    // from already-known nodes
    for (const auto& kv : mNodes) names.insert(kv.first);
    // from devices
    auto addName = [&](const IdString& nodeName) {
        if (nodeName.valid() && names.find(nodeName) == names.end()) {
            ensureNode(nodeName);
            names.insert(nodeName);
        }
    };

    for (const auto& res : mMetalResistors) {
        addName(res.n1);
        addName(res.n2);
    }
    for (const auto& res : mViaResistors) {
        addName(res.n1);
        addName(res.n2);
    }
    for (const auto& res : mTsvResistors) {
        addName(res.n1);
        addName(res.n2);
    }
    for (const auto& res : mPkgResistors) {
        addName(res.n1);
        addName(res.n2);
    }
    for (const auto& src : mVsrcs) {
        addName(src.fromNode);
        addName(src.toNode);
    }
    for (const auto& src : mIsrcs) {
        addName(src.fromNode);
        addName(src.toNode);
    }
    // Always ensure ground marker exists if referenced by any element
    if (names.count(GND) == 0) {
        if (refersToGround()) {
            ensureNode(GND);
        }
    }
}

bool CircuitGraph::refersToGround() const {
    auto ref0 = [](const IdString& s) { return s == "GND"; };
    for (const auto& res : mMetalResistors)
        if (ref0(res.n1) || ref0(res.n2)) return true;
    for (const auto& res : mViaResistors)
        if (ref0(res.n1) || ref0(res.n2)) return true;
    for (const auto& res : mTsvResistors)
        if (ref0(res.n1) || ref0(res.n2)) return true;
    for (const auto& res : mPkgResistors)
        if (ref0(res.n1) || ref0(res.n2)) return true;
    for (const auto& src : mVsrcs)
        if (ref0(src.fromNode) || ref0(src.toNode)) return true;
    for (const auto& src : mIsrcs)
        if (ref0(src.fromNode) || ref0(src.toNode)) return true;
    return false;
}

void CircuitGraph::validateReadyForMna() const {
    if (mVsrcs.empty()) {
        throw std::runtime_error(
          "No voltage sources found. PDN DC solve requires at least one "
          "voltage source (e.g., VDD or via 0V sources).");
    }

    auto checkR = [](const IdString& id, double resistance) {
        if (!(resistance > 0.0)) {
            throw std::runtime_error(
              "Non-positive resistance encountered in " + id.str() + ": " +
              std::to_string(resistance));
        }
    };

    for (const auto& res : mMetalResistors) checkR(res.name, res.R);
    for (const auto& res : mViaResistors) checkR(res.name, res.R);
    for (const auto& res : mTsvResistors) checkR(res.name, res.R);
    for (const auto& res : mPkgResistors) checkR(res.name, res.R);
}

void CircuitGraph::purgeParallelElements() {
    // 1. Resistive network (metal, via, package)
    mergeParallelResistors(mMetalResistors);
    mergeParallelResistors(mViaResistors);
    mergeParallelResistors(mTsvResistors);
    mergeParallelResistors(mPkgResistors);

    // 2. Current sources
    mergeParallelIsrcs(mIsrcs);

    // 3. Voltage sources
    dedupVsrcs(mVsrcs);
}

// Purge nodes that have no incident elements (degree == 0).
// Returns the number of nodes removed.
std::size_t CircuitGraph::purgeIsolatedNodes() {
    if (mNodes.empty()) {
        return 0;
    }

    using DegreeT = std::uint32_t;

    // Degree map for existing nodes only
    IdString::Map<DegreeT> degree;
    degree.reserve(mNodes.size());

    // Assumption: NodeMap is keyed by node name (IdString), matching element
    // endpoints
    for (const auto& kv : mNodes) {
        degree.emplace(kv.first, DegreeT{0});
    }

    auto bumpIfPresent = [&](const IdString& nodeName) {
        auto it = degree.find(nodeName);
        if (it != degree.end()) {
            ++it->second;
        }
        // If an element references a node that is not in mNodes, we ignore it
        // here. (This function is only about removing existing degree-0
        // nodes.)
    };

    // Count incident edges for each node
    for (const auto& e : mMetalResistors) {
        bumpIfPresent(e.n1);
        bumpIfPresent(e.n2);
    }
    for (const auto& e : mViaResistors) {
        bumpIfPresent(e.n1);
        bumpIfPresent(e.n2);
    }
    for (const auto& e : mTsvResistors) {
        bumpIfPresent(e.n1);
        bumpIfPresent(e.n2);
    }
    for (const auto& e : mPkgResistors) {
        bumpIfPresent(e.n1);
        bumpIfPresent(e.n2);
    }
    for (const auto& e : mVsrcs) {
        bumpIfPresent(e.fromNode);
        bumpIfPresent(e.toNode);
    }
    for (const auto& e : mIsrcs) {
        bumpIfPresent(e.fromNode);
        bumpIfPresent(e.toNode);
    }

    // Remove nodes with degree 0
    std::size_t removed = 0;
    for (auto it = mNodes.begin(); it != mNodes.end();) {
        const IdString& nodeName = it->first;

        auto          dit = degree.find(nodeName);
        const DegreeT deg = (dit == degree.end()) ? DegreeT{0} : dit->second;

        if (deg == 0) {
            auto toErase = it++;
            mNodes.erase(toErase);
            ++removed;
        } else {
            ++it;
        }
    }

    return removed;
}
} // namespace pdnsol
