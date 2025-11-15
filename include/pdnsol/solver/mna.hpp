#pragma once
#include <optional>
#include <unordered_map>

#include <Eigen/Sparse>
#include <Eigen/SparseLU>

#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {
// -------------------------------
// MNA system and solution
// -------------------------------

struct MNASystem {
    Eigen::SparseMatrix<double, Eigen::ColMajor, int> A;
    Eigen::VectorXd b;
    // node name -> index in voltage unknowns
    std::unordered_map<IdString, int, IdString::Hash> nodeIndex;
    // vsrc mId   -> index in current unknowns (offset N)
    std::unordered_map<IdString, int, IdString::Hash> vsrcIndex;
};

struct MNASolution {
    std::unordered_map<IdString, double> voltages; // node_name -> V
    std::unordered_map<IdString, double>
      vsrc_currents; // vsrc_mId  -> current (A), sign from n_plus to n_minus
    Eigen::VectorXd x_full;
    std::unordered_map<IdString, int, IdString::Hash> nodeIndex;
    std::unordered_map<IdString, int, IdString::Hash> vsrcIndex;
};

// -------------------------------
// Internal helpers for indexing
// -------------------------------

struct Indices {
    std::unordered_map<IdString, int, IdString::Hash> nodeIndex;
    std::unordered_map<IdString, int, IdString::Hash> vsrcIndex;
    // We keep a simple reference list to mVsrcs in their vector order for
    // stamping (we won't store pointers to avomId dangling; stamping reads
    // from circ.mVsrcs)
};

inline Indices build_indices(const CircuitGraph& circ) {
    // Deterministic node order: sorted names, excluding ground "0"
    std::vector<IdString> node_names;
    node_names.reserve(circ.mNodes.size());
    for (const auto& kv : circ.mNodes) {
        if (kv.first != "0") node_names.push_back(kv.first);
    }
    std::sort(node_names.begin(), node_names.end());
    std::unordered_map<IdString, int, IdString::Hash> nodeIndex;
    nodeIndex.reserve(node_names.size());
    for (int i = 0; i < static_cast<int>(node_names.size()); ++i) {
        nodeIndex.emplace(node_names[i], i);
    }

    // Voltage sources in given order
    std::unordered_map<IdString, int, IdString::Hash> vsrcIndex;
    vsrcIndex.reserve(circ.mVsrcs.size());
    for (int k = 0; k < static_cast<int>(circ.mVsrcs.size()); ++k) {
        vsrcIndex.emplace(circ.mVsrcs[k].mId, k);
    }

    return Indices{std::move(nodeIndex), std::move(vsrcIndex)};
}

// -------------------------------
// Assembly (stamping) and solve
// -------------------------------

inline MNASystem assemble_mna(CircuitGraph& circ) {
    circ.ensureAllReferencedNodesExist();
    circ.validateReadyForMna();

    Indices ind = build_indices(circ);
    const int N = static_cast<int>(ind.nodeIndex.size());
    const int M = static_cast<int>(ind.vsrcIndex.size());
    const int dim = N + M;

    // Triplet list for sparse assembly
    std::vector<Eigen::Triplet<double, int>> trips;
    trips.reserve(static_cast<std::size_t>(4) *
                    (circ.mMetalResistors.size() + circ.mViaResistors.size() +
                     circ.mPkgResistors.size()) +
                  static_cast<std::size_t>(4) * M);

    Eigen::VectorXd b = Eigen::VectorXd::Zero(dim);

    auto mIdx = [&](const IdString& n) -> std::optional<int> {
        if (n == "0") return std::nullopt;
        auto it = ind.nodeIndex.find(n);
        if (it == ind.nodeIndex.end())
            return std::nullopt; // unknown node -> ignored (shouldn't happen
                                 // if ensured)
        return it->second;
    };

    auto stamp_G = [&](const IdString& mN1, const IdString& mN2, double g) {
        auto i = mIdx(mN1);
        auto j = mIdx(mN2);
        if (i) trips.emplace_back(*i, *i, g);
        if (j) trips.emplace_back(*j, *j, g);
        if (i && j) {
            trips.emplace_back(*i, *j, -g);
            trips.emplace_back(*j, *i, -g);
        }
    };

    auto stamp_I =
      [&](const IdString& n_plus, const IdString& n_minus, double I) {
          auto i = mIdx(n_plus);
          auto j = mIdx(n_minus);
          if (i)
              b(*i) -=
                I; // current out of node (into element) is negative in KCL RHS
          if (j) b(*j) += I;
      };

    auto stamp_V = [&](const Vsrc& vsrc) {
        auto it = ind.vsrcIndex.find(vsrc.mId);
        if (it == ind.vsrcIndex.end()) {
            throw std::runtime_error("Voltage source mId not indexed: " +
                                     vsrc.mId.str());
        }
        const int k = it->second;
        const int row = N + k;

        auto npp = mIdx(vsrc.mNPlus);
        auto nmm = mIdx(vsrc.mNMinus);

        // KCL coupling (top-right and bottom-left blocks)
        if (npp) {
            trips.emplace_back(*npp, row, +1.0);
            trips.emplace_back(row, *npp, +1.0);
        }
        if (nmm) {
            trips.emplace_back(*nmm, row, -1.0);
            trips.emplace_back(row, *nmm, -1.0);
        }

        // KVL equation RHS
        b(row) = vsrc.mV;
    };

    // Stamp resistors
    for (const auto& r : circ.mMetalResistors) {
        const double g = 1.0 / r.mR;
        stamp_G(r.mN1, r.mN2, g);
    }
    for (const auto& r : circ.mViaResistors) {
        const double g = 1.0 / r.mR;
        stamp_G(r.mN1, r.mN2, g);
    }
    for (const auto& r : circ.mPkgResistors) {
        const double g = 1.0 / r.mR;
        stamp_G(r.mN1, r.mN2, g);
    }

    // Stamp current sources
    for (const auto& isrc : circ.mIsrcs) {
        stamp_I(isrc.mNPlus, isrc.mNMinus, isrc.mI);
    }

    // Stamp voltage sources
    for (const auto& vs : circ.mVsrcs) {
        stamp_V(vs);
    }

    // Build sparse matrix
    Eigen::SparseMatrix<double, Eigen::ColMajor, int> A(dim, dim);
    A.setFromTriplets(trips.begin(), trips.end()); // duplicates are summed
    A.makeCompressed();

    MNASystem sys;
    sys.A = std::move(A);
    sys.b = std::move(b);
    sys.nodeIndex = std::move(ind.nodeIndex);
    sys.vsrcIndex = std::move(ind.vsrcIndex);
    return sys;
}

} // namespace pdnsol