#include "pdnsol/solver/solver_basic.hpp"

namespace pdnsol {

MNASolution solveMNA(const MNASystem& sys) {
    const auto& A = sys.mA;
    const auto& b = sys.mB;

    Eigen::SparseLU<Eigen::SparseMatrix<double, Eigen::ColMajor, int>> solver;
    solver.analyzePattern(A);
    solver.factorize(A);
    if (solver.info() != Eigen::Success) {
        throw std::runtime_error("MNA matrix factorization failed; system may "
                                 "be singular or ill-conditioned.");
    }
    Eigen::VectorXd x = solver.solve(b);
    if (solver.info() != Eigen::Success) {
        throw std::runtime_error(
          "MNA solve failed; system may be singular or ill-conditioned.");
    }

    const int N = static_cast<int>(sys.mNodeIndex.size());

    IdString::Map<ScalarType> nodeVoltages;
    nodeVoltages.reserve(sys.mNodeIndex.size());
    for (const auto& kv : sys.mNodeIndex) {
        nodeVoltages.emplace(kv.first, x(kv.second));
    }

    IdString::Map<ScalarType> vsrcCurrents;
    vsrcCurrents.reserve(sys.mVsrcIndex.size());
    for (const auto& kv : sys.mVsrcIndex) {
        vsrcCurrents.emplace(kv.first, x(N + kv.second));
    }

    MNASolution sol;
    sol.mVoltages = std::move(nodeVoltages);
    sol.mVsrcCurrents = std::move(vsrcCurrents);
    sol.mXFull = std::move(x);
    sol.mNodeIndex = sys.mNodeIndex;
    sol.mVsrcIndex = sys.mVsrcIndex;
    return sol;
}

} // namespace pdnsol