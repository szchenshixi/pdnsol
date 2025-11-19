#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/fixed_point_number.hpp"

#include <stdexcept>
#include <unordered_set>

namespace pdnsol {
const NodeMap& CircuitGraph::allNodes() const { return mNodes; }

Node& CircuitGraph::ensureNode(const IdString& name, int net,
                               std::optional<double> x,
                               std::optional<double> y) {
    if (name == "GND") {
        auto it = mNodes.find(name);
        if (it == mNodes.end()) {
            Node groundNode;
            groundNode.mName = IdString("GND");
            it = mNodes.emplace(name, std::move(groundNode)).first;
        }
        return it->second;
    }

    auto it = mNodes.find(name);
    if (it != mNodes.end()) {
        Node& node = it->second;
        if (net && !node.mNet) node.mNet = net;
        if (x && !node.mXMicros) node.mXMicros = FPN::toRep(*x);
        if (y && !node.mYMicros) node.mYMicros = FPN::toRep(*y);
        return node;
    }

    Node newNode;
    newNode.mName = name;
    newNode.mNet = net;
    newNode.mXMicros = x ? *x : -1;
    newNode.mYMicros = y ? *y : -1;
    auto [insertIt, _] = mNodes.emplace(name, std::move(newNode));
    return insertIt->second;
}

std::size_t CircuitGraph::countVoltageSources() const { return mVsrcs.size(); }

void CircuitGraph::ensureAllReferencedNodesExist() {
    std::unordered_set<IdString, IdString::Hash> names;
    // from already-known nodes
    for (const auto& kv : mNodes)
        names.insert(kv.first);
    // from devices
    auto addName = [&](const IdString& nodeName) {
        if (nodeName.valid() && names.find(nodeName) == names.end()) {
            ensureNode(nodeName);
            names.insert(nodeName);
        }
    };

    for (const auto& res : mMetalResistors) {
        addName(res.mN1);
        addName(res.mN2);
    }
    for (const auto& res : mViaResistors) {
        addName(res.mN1);
        addName(res.mN2);
    }
    for (const auto& res : mPkgResistors) {
        addName(res.mN1);
        addName(res.mN2);
    }
    for (const auto& src : mVsrcs) {
        addName(src.mFromNode);
        addName(src.mToNode);
    }
    for (const auto& src : mIsrcs) {
        addName(src.mFromNode);
        addName(src.mToNode);
    }
    // Always ensure ground marker exists if referenced by any element
    if (names.count(IdString("GND")) == 0) {
        if (refersToGround()) { ensureNode(IdString("GND")); }
    }
}

bool CircuitGraph::refersToGround() const {
    auto ref0 = [](const IdString& s) { return s == "GND"; };
    for (const auto& res : mMetalResistors)
        if (ref0(res.mN1) || ref0(res.mN2)) return true;
    for (const auto& res : mViaResistors)
        if (ref0(res.mN1) || ref0(res.mN2)) return true;
    for (const auto& res : mPkgResistors)
        if (ref0(res.mN1) || ref0(res.mN2)) return true;
    for (const auto& src : mVsrcs)
        if (ref0(src.mFromNode) || ref0(src.mToNode)) return true;
    for (const auto& src : mIsrcs)
        if (ref0(src.mFromNode) || ref0(src.mToNode)) return true;
    return false;
}

void CircuitGraph::validateReadyForMna() const {
    if (countVoltageSources() == 0) {
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

    for (const auto& res : mMetalResistors)
        checkR(res.mName, res.mR);
    for (const auto& res : mViaResistors)
        checkR(res.mName, res.mR);
    for (const auto& res : mPkgResistors)
        checkR(res.mName, res.mR);
}
} // namespace pdnsol