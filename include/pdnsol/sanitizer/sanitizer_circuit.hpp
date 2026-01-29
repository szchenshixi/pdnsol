#pragma once

#include <string>
#include <vector>

#include "pdnsol/struct/circuit.hpp"
#include "pdnsol/utils/id_string.hpp"

namespace pdnsol {
class CircuitConnectivityChecker {
  private:
    struct GraphNode {
        IdString::Set neighbors;
        bool                    visited = false;
        IdString                id;
    };

    IdString::Map<GraphNode> nodeGraph;

    void                               buildGraph(const CircuitGraph& circuit);
    std::vector<std::vector<IdString>> findConnectedComponents();
    void                               resetVisited();

  public:
    struct IsolationDiagnostic {
        struct IsolatedComponent {
            std::vector<IdString> nodes;
            std::vector<IdString> resistors;
            std::vector<IdString> vias;
            std::vector<IdString> tsvs;
            std::vector<IdString> pkgs;
            std::vector<IdString> vsrcs;
            std::vector<IdString> isrcs;
            bool                  hasPowerSource = false;
        };

        std::vector<IsolatedComponent> isolatedComponents;
        std::vector<IsolatedComponent> poweredComponents;
        size_t                         totalComponents = 0;
    };

    struct ShortCircuitInfo {
        IdString    node1;
        IdString    node2;
        IdString    componentName;
        std::string componentType;
    };

    // Main isolation checking function
    IsolationDiagnostic checkIsolation(const CircuitGraph& circuit);

    // Simplified isolation check
    std::vector<IdString> findIsolatedNodes(const CircuitGraph& circuit);

    // Open circuit detection
    std::vector<IdString> findOpenCircuits(const CircuitGraph& circuit);

    // Short circuit detection
    std::vector<ShortCircuitInfo>
    findShortCircuits(const CircuitGraph& circuit);

    // Generate comprehensive report
    std::string generateReport(const CircuitGraph& circuit);
};
} // namespace pdnsol