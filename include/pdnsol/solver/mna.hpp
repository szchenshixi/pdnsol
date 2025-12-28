#ifndef MNA_HPP_
#define MNA_HPP_

#include <Eigen/Sparse>

#include "pdnsol/common.hpp"
#include "pdnsol/struct/circuit.hpp"

namespace pdnsol {

// -------------------------------
// Modified Nodal Analysis (MNA) system
// -------------------------------

struct MNASystem {
    Eigen::SparseMatrix<double, Eigen::ColMajor, int> mA;
    Eigen::VectorXd mB;
    IdString::Map<IndexType> mNodeIndex;
    IdString::Map<IndexType> mVsrcIndex;
};

struct MNASolution {
    IdString::Map<ScalarType> mVoltages;
    IdString::Map<ScalarType> mVsrcCurrents;
    Eigen::VectorXd mXFull;
    IdString::Map<IndexType> mNodeIndex;
    IdString::Map<IndexType> mVsrcIndex;
};

// -------------------------------
// Internal helpers for indexing
// -------------------------------

struct Indices {
    IdString::Map<IndexType> mNodeIndex;
    IdString::Map<IndexType> mVsrcIndex;
};

Indices buildIndices(const CircuitGraph& circ);

// -------------------------------
// Assembly (stamping) for solve
// -------------------------------

MNASystem assembleMNA(CircuitGraph& circ);

} // namespace pdnsol

#endif // MNA_HPP_