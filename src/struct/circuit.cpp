#include <stdexcept>

#include "pdnsol/struct/circuit.hpp"

namespace pdnsol {

const NodeMap& CircuitGraph::allNodes() const { return mNodes; }

Node& CircuitGraph::ensureNode(const IdString& name, IdString net,
                               IdString layer, std::optional<double> x,
                               std::optional<double> y) {
    if (name == "0") {
        auto it = mNodes.find(name);
        if (it == mNodes.end()) {
            Node groundNode;
            groundNode.mName = IdString("0");
            it = mNodes.emplace(name, std::move(groundNode)).first;
        }
        return it->second;
    }

    auto it = mNodes.find(name);
    if (it != mNodes.end()) {
        Node& node = it->second;
        if (net && !node.mNet) node.mNet = net;
        if (layer && !node.mLayer) node.mLayer = layer;
        if (x && !node.mX) node.mX = *x;
        if (y && !node.mY) node.mY = *y;
        return node;
    }

    Node newNode;
    newNode.mName = name;
    newNode.mNet = net;
    newNode.mLayer = layer;
    newNode.mX = x ? *x : -1;
    newNode.mY = y ? *y : -1;
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
        addName(src.mNPlus);
        addName(src.mNMinus);
    }
    for (const auto& src : mIsrcs) {
        addName(src.mNPlus);
        addName(src.mNMinus);
    }
    // Always ensure ground marker exists if referenced by any element
    if (names.count(IdString("0")) == 0) {
        if (refersToGround()) { ensureNode(IdString("0")); }
    }
}

bool CircuitGraph::refersToGround() const {
    auto ref0 = [](const IdString& s) { return s == "0"; };
    for (const auto& res : mMetalResistors)
        if (ref0(res.mN1) || ref0(res.mN2)) return true;
    for (const auto& res : mViaResistors)
        if (ref0(res.mN1) || ref0(res.mN2)) return true;
    for (const auto& res : mPkgResistors)
        if (ref0(res.mN1) || ref0(res.mN2)) return true;
    for (const auto& src : mVsrcs)
        if (ref0(src.mNPlus) || ref0(src.mNMinus)) return true;
    for (const auto& src : mIsrcs)
        if (ref0(src.mNPlus) || ref0(src.mNMinus)) return true;
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
        checkR(res.mId, res.mR);
    for (const auto& res : mViaResistors)
        checkR(res.mId, res.mR);
    for (const auto& res : mPkgResistors)
        checkR(res.mId, res.mR);
}
} // namespace pdnsol