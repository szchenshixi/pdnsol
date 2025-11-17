#include <algorithm>
#include <stdexcept>
#include <vector>

#include "pdnsol/solver/mna.hpp"

namespace pdnsol {

Indices buildIndices(const CircuitGraph& circ) {
    std::vector<IdString> nodeNames;
    nodeNames.reserve(circ.mNodes.size());
    for (const auto& kv : circ.mNodes) {
        if (kv.first != "GND") nodeNames.push_back(kv.first);
    }
    std::sort(nodeNames.begin(), nodeNames.end());
    IntMap nodeIndex;
    nodeIndex.reserve(nodeNames.size());
    for (IndexType i = 0; i < static_cast<IndexType>(nodeNames.size()); ++i) {
        nodeIndex.emplace(nodeNames[i], i);
    }

    IntMap vsrcIndex;
    vsrcIndex.reserve(circ.mVsrcs.size());
    for (IndexType k = 0; k < static_cast<IndexType>(circ.mVsrcs.size()); ++k) {
        vsrcIndex.emplace(circ.mVsrcs[k].mName, k);
    }

    return Indices{std::move(nodeIndex), std::move(vsrcIndex)};
}

MNASystem assembleMNA(CircuitGraph& circ) {
    circ.ensureAllReferencedNodesExist();
    circ.validateReadyForMna();

    Indices indices = buildIndices(circ);
    const int numNodes = static_cast<int>(indices.mNodeIndex.size());
    const int numVsrcs = static_cast<int>(indices.mVsrcIndex.size());
    const int dim = numNodes + numVsrcs;

    std::vector<Eigen::Triplet<ScalarType, IndexType>> triplets;
    // Each resistor/vsrc contributes four entries to the conductance matrix
    triplets.reserve(static_cast<std::size_t>(4) *
                       (circ.mMetalResistors.size() +
                        circ.mViaResistors.size() +
                        circ.mPkgResistors.size()) +
                     static_cast<std::size_t>(4) * numVsrcs);

    Eigen::VectorXd bVector = Eigen::VectorXd::Zero(dim);

    auto getNodeIndex = [&](const IdString& nodeName) -> IndexType {
        if (nodeName == "GND") return -1;
        auto it = indices.mNodeIndex.find(nodeName);
        if (it == indices.mNodeIndex.end()) return -1;
        return it->second;
    };

    auto stampConductance =
      [&](const IdString& node1, const IdString& node2, ScalarType conductance) {
        IndexType idx1 = getNodeIndex(node1);
        IndexType idx2 = getNodeIndex(node2);
          if (idx1 > -1) triplets.emplace_back(idx1, idx1, conductance);
          if (idx2 > -1) triplets.emplace_back(idx2, idx2, conductance);
          if (idx1 > -1 && idx2 > -1) {
              triplets.emplace_back(idx1, idx2, -conductance);
              triplets.emplace_back(idx2, idx1, -conductance);
          }
      };

    auto stampCurrent =
      [&](const IdString& fromNode, const IdString& toNode, ScalarType current) {
        IndexType fromIdx = getNodeIndex(fromNode);
        IndexType toIdx = getNodeIndex(toNode);
          if (fromIdx > -1) bVector(fromIdx) -= current;
          if (toIdx > -1) bVector(toIdx) += current;
      };

    auto stampVoltageSource = [&](const Vsrc& voltageSource) {
        auto it = indices.mVsrcIndex.find(voltageSource.mName);
        if (it == indices.mVsrcIndex.end()) {
            throw std::runtime_error("Voltage source mId not indexed: " +
                                     voltageSource.mName.str());
        }
        const IndexType vsrcIdx = it->second;
        const IndexType row = numNodes + vsrcIdx;

        IndexType fromIdx = getNodeIndex(voltageSource.mFromNode);
        IndexType toIdx = getNodeIndex(voltageSource.mToNode);

        if (fromIdx > -1) {
            triplets.emplace_back(fromIdx, row, +1.0);
            triplets.emplace_back(row, fromIdx, +1.0);
        }
        if (toIdx > -1) {
            triplets.emplace_back(toIdx, row, -1.0);
            triplets.emplace_back(row, toIdx, -1.0);
        }

        bVector(row) = voltageSource.mV;
    };

    for (const auto& resistor : circ.mMetalResistors) {
        const ScalarType conductance = 1.0 / resistor.mR;
        stampConductance(resistor.mN1, resistor.mN2, conductance);
    }
    for (const auto& resistor : circ.mViaResistors) {
        const ScalarType conductance = 1.0 / resistor.mR;
        stampConductance(resistor.mN1, resistor.mN2, conductance);
    }
    for (const auto& resistor : circ.mPkgResistors) {
        const ScalarType conductance = 1.0 / resistor.mR;
        stampConductance(resistor.mN1, resistor.mN2, conductance);
    }

    for (const auto& currentSource : circ.mIsrcs) {
        stampCurrent(
          currentSource.mFromNode, currentSource.mToNode, currentSource.mI);
    }

    for (const auto& voltageSource : circ.mVsrcs) {
        stampVoltageSource(voltageSource);
    }

    Eigen::SparseMatrix<ScalarType, Eigen::ColMajor, IndexType> aMatrix(dim, dim);
    aMatrix.setFromTriplets(triplets.begin(), triplets.end());
    aMatrix.makeCompressed();

    MNASystem system;
    system.mA = std::move(aMatrix);
    system.mB = std::move(bVector);
    system.mNodeIndex = std::move(indices.mNodeIndex);
    system.mVsrcIndex = std::move(indices.mVsrcIndex);
    return system;
}

} // namespace pdnsol