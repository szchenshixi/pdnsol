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
    IntMap mNodeIndex;
    IntMap mVsrcIndex;
};

struct MNASolution {
    DoubleMap mVoltages;
    DoubleMap mVsrcCurrents;
    Eigen::VectorXd mXFull;
    IntMap mNodeIndex;
    IntMap mVsrcIndex;
};

// -------------------------------
// Internal helpers for indexing
// -------------------------------

struct Indices {
    IntMap mNodeIndex;
    IntMap mVsrcIndex;
};

Indices buildIndices(const CircuitGraph& circ);

// -------------------------------
// Assembly (stamping) for solve
// -------------------------------

MNASystem assembleMNA(CircuitGraph& circ);

} // namespace pdnsol

#endif // MNA_HPP_