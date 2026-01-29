#include "pdnsol/sanitizer/sanitizer_circuit.hpp"

#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pdnsol {
// Build the undirected graph from circuit components
void CircuitConnectivityChecker::buildGraph(const CircuitGraph& circuit) {
    // Clear existing graph
    nodeGraph.clear();

    // Add all nodes
    for (const auto& nodePair : circuit.mNodes) {
        GraphNode& graphNode = nodeGraph[nodePair.first];
        graphNode.id         = nodePair.first;
        graphNode.visited    = false;
        graphNode.neighbors.clear();
    }

    // Add edges from metal resistors
    for (const auto& res : circuit.mMetalResistors) {
        if (nodeGraph.count(res.n1) && nodeGraph.count(res.n2)) {
            nodeGraph[res.n1].neighbors.insert(res.n2);
            nodeGraph[res.n2].neighbors.insert(res.n1);
        }
    }

    // Add edges from via resistors
    for (const auto& res : circuit.mViaResistors) {
        if (nodeGraph.count(res.n1) && nodeGraph.count(res.n2)) {
            nodeGraph[res.n1].neighbors.insert(res.n2);
            nodeGraph[res.n2].neighbors.insert(res.n1);
        }
    }

    // Add edges from tsv resistors
    for (const auto& res : circuit.mTsvResistors) {
        if (nodeGraph.count(res.n1) && nodeGraph.count(res.n2)) {
            nodeGraph[res.n1].neighbors.insert(res.n2);
            nodeGraph[res.n2].neighbors.insert(res.n1);
        }
    }

    // Add edges from package resistors
    for (const auto& res : circuit.mPkgResistors) {
        if (nodeGraph.count(res.n1) && nodeGraph.count(res.n2)) {
            nodeGraph[res.n1].neighbors.insert(res.n2);
            nodeGraph[res.n2].neighbors.insert(res.n1);
        }
    }

    // Add edges from voltage sources (treat as connections)
    for (const auto& src : circuit.mVsrcs) {
        if (nodeGraph.count(src.fromNode) && nodeGraph.count(src.toNode)) {
            nodeGraph[src.fromNode].neighbors.insert(src.toNode);
            nodeGraph[src.toNode].neighbors.insert(src.fromNode);
        }
    }

    // Add edges from current sources (they still conduct)
    for (const auto& src : circuit.mIsrcs) {
        if (nodeGraph.count(src.fromNode) && nodeGraph.count(src.toNode)) {
            nodeGraph[src.fromNode].neighbors.insert(src.toNode);
            nodeGraph[src.toNode].neighbors.insert(src.fromNode);
        }
    }
}

// Find all connected components using BFS
std::vector<std::vector<IdString>>
CircuitConnectivityChecker::findConnectedComponents() {
    std::vector<std::vector<IdString>> components;

    for (auto& nodePair : nodeGraph) {
        GraphNode& node = nodePair.second;
        if (!node.visited) {
            std::vector<IdString> component;
            std::queue<IdString>  bfsQueue;

            bfsQueue.push(node.id);
            node.visited = true;

            while (!bfsQueue.empty()) {
                IdString currentId = bfsQueue.front();
                bfsQueue.pop();
                component.push_back(currentId);

                for (const auto& neighborId : nodeGraph[currentId].neighbors) {
                    if (!nodeGraph[neighborId].visited) {
                        nodeGraph[neighborId].visited = true;
                        bfsQueue.push(neighborId);
                    }
                }
            }

            if (!component.empty()) {
                components.push_back(std::move(component));
            }
        }
    }

    return components;
}

// Reset visited flags
void CircuitConnectivityChecker::resetVisited() {
    for (auto& nodePair : nodeGraph) {
        nodePair.second.visited = false;
    }
}

// Check for isolated components and return diagnostic information
CircuitConnectivityChecker::IsolationDiagnostic
CircuitConnectivityChecker::checkIsolation(const CircuitGraph& circuit) {
    // Build the graph
    buildGraph(circuit);

    // Find all connected components
    auto components = findConnectedComponents();

    // Create diagnostic result
    IsolationDiagnostic diagnostic;
    diagnostic.totalComponents = components.size();

    // For each component, collect detailed information
    for (const auto& componentNodes : components) {
        IsolationDiagnostic::IsolatedComponent compInfo;
        compInfo.nodes = componentNodes;

        // Create a set for fast membership check
        IdString::Set nodeSet(componentNodes.begin(),
                                        componentNodes.end());

        // Check for power sources in this component
        for (const auto& vsrc : circuit.mVsrcs) {
            if (nodeSet.count(vsrc.fromNode) || nodeSet.count(vsrc.toNode)) {
                compInfo.vsrcs.push_back(vsrc.name);
                compInfo.hasPowerSource = true;
            }
        }

        // Check for current sources
        for (const auto& isrc : circuit.mIsrcs) {
            if (nodeSet.count(isrc.fromNode) || nodeSet.count(isrc.toNode)) {
                compInfo.isrcs.push_back(isrc.name);
            }
        }

        // Check for resistors in this component
        for (const auto& res : circuit.mMetalResistors) {
            if (nodeSet.count(res.n1) && nodeSet.count(res.n2)) {
                compInfo.resistors.push_back(res.name);
            }
        }

        // Check for vias in this component
        for (const auto& via : circuit.mViaResistors) {
            if (nodeSet.count(via.n1) && nodeSet.count(via.n2)) {
                compInfo.vias.push_back(via.name);
            }
        }

        // Check for tsvs in this component
        for (const auto& tsv : circuit.mTsvResistors) {
            if (nodeSet.count(tsv.n1) && nodeSet.count(tsv.n2)) {
                compInfo.tsvs.push_back(tsv.name);
            }
        }

        // Check for package resistors in this component
        for (const auto& pkg : circuit.mPkgResistors) {
            if (nodeSet.count(pkg.n1) && nodeSet.count(pkg.n2)) {
                compInfo.pkgs.push_back(pkg.name);
            }
        }

        // Classify as isolated or powered
        if (compInfo.hasPowerSource) {
            diagnostic.poweredComponents.push_back(std::move(compInfo));
        } else {
            diagnostic.isolatedComponents.push_back(std::move(compInfo));
        }
    }

    resetVisited();
    return diagnostic;
}

// Simplified check - just return isolated nodes
std::vector<IdString>
CircuitConnectivityChecker::findIsolatedNodes(const CircuitGraph& circuit) {
    buildGraph(circuit);

    auto                  components = findConnectedComponents();
    std::vector<IdString> isolatedNodes;

    // Check which components have power sources
    for (const auto& component : components) {
        bool hasPowerSource = false;

        // Create set for this component
        std::unordered_set<IdString, IdString::Hash> nodeSet(component.begin(),
                                                             component.end());

        // Check for voltage sources
        for (const auto& vsrc : circuit.mVsrcs) {
            if (nodeSet.count(vsrc.fromNode) || nodeSet.count(vsrc.toNode)) {
                hasPowerSource = true;
                break;
            }
        }

        // If no power source, all nodes in this component are isolated
        if (!hasPowerSource) {
            isolatedNodes.insert(
              isolatedNodes.end(), component.begin(), component.end());
        }
    }

    resetVisited();
    return isolatedNodes;
}

// Check for open circuits (nodes with no connections)
std::vector<IdString>
CircuitConnectivityChecker::findOpenCircuits(const CircuitGraph& circuit) {
    std::vector<IdString> openNodes;

    for (const auto& nodePair : circuit.mNodes) {
        bool hasConnection = false;

        // Check metal resistors
        for (const auto& res : circuit.mMetalResistors) {
            if (res.n1 == nodePair.first || res.n2 == nodePair.first) {
                hasConnection = true;
                break;
            }
        }

        // Check via resistors
        if (!hasConnection) {
            for (const auto& via : circuit.mViaResistors) {
                if (via.n1 == nodePair.first || via.n2 == nodePair.first) {
                    hasConnection = true;
                    break;
                }
            }
        }

        // Check tsv resistors
        if (!hasConnection) {
            for (const auto& tsv : circuit.mTsvResistors) {
                if (tsv.n1 == nodePair.first || tsv.n2 == nodePair.first) {
                    hasConnection = true;
                    break;
                }
            }
        }

        // Check package resistors
        if (!hasConnection) {
            for (const auto& pkg : circuit.mPkgResistors) {
                if (pkg.n1 == nodePair.first || pkg.n2 == nodePair.first) {
                    hasConnection = true;
                    break;
                }
            }
        }

        // Check voltage sources
        if (!hasConnection) {
            for (const auto& vsrc : circuit.mVsrcs) {
                if (vsrc.fromNode == nodePair.first ||
                    vsrc.toNode == nodePair.first) {
                    hasConnection = true;
                    break;
                }
            }
        }

        // Check current sources
        if (!hasConnection) {
            for (const auto& isrc : circuit.mIsrcs) {
                if (isrc.fromNode == nodePair.first ||
                    isrc.toNode == nodePair.first) {
                    hasConnection = true;
                    break;
                }
            }
        }

        if (!hasConnection) {
            openNodes.push_back(nodePair.first);
        }
    }

    return openNodes;
}

// Check for short circuits (zero resistance paths)
std::vector<CircuitConnectivityChecker::ShortCircuitInfo>
CircuitConnectivityChecker::findShortCircuits(const CircuitGraph& circuit) {
    std::vector<ShortCircuitInfo> shorts;

    // Check metal resistors
    for (const auto& res : circuit.mMetalResistors) {
        if (res.R == 0.0) {
            shorts.push_back({res.n1, res.n2, res.name, "metal"});
        }
    }

    // Check via resistors
    for (const auto& via : circuit.mViaResistors) {
        if (via.R == 0.0) {
            shorts.push_back({via.n1, via.n2, via.name, "via"});
        }
    }

    // Check tsv resistors
    for (const auto& tsv : circuit.mTsvResistors) {
        if (tsv.R == 0.0) {
            shorts.push_back({tsv.n1, tsv.n2, tsv.name, "tsv"});
        }
    }

    // Check package resistors
    for (const auto& pkg : circuit.mPkgResistors) {
        if (pkg.R == 0.0) {
            shorts.push_back({pkg.n1, pkg.n2, pkg.name, "package"});
        }
    }

    // [Deprecated] Voltage sources with 0V are allowed shorts
    // for (const auto& vsrc : circuit.mVsrcs) {
    //     if (vsrc.V > 0.0) {
    //         shorts.push_back(
    //           {vsrc.fromNode, vsrc.toNode, vsrc.name, "vsrc"});
    //     }
    // }

    return shorts;
}

// Generate a connectivity report
std::string
CircuitConnectivityChecker::generateReport(const CircuitGraph& circuit) {
    std::stringstream report;

    auto isolation = checkIsolation(circuit);
    auto openNodes = findOpenCircuits(circuit);
    auto shorts    = findShortCircuits(circuit);

    report << "=== Circuit Connectivity Report ===\n\n";

    report << "Connected Components: " << isolation.totalComponents << "\n";
    report << "Powered Components: " << isolation.poweredComponents.size()
           << "\n";
    report << "Isolated Components: " << isolation.isolatedComponents.size()
           << "\n\n";

    // Report isolated components
    if (!isolation.isolatedComponents.empty()) {
        report << "=== ISOLATED COMPONENTS ===\n";
        for (size_t i = 0; i < isolation.isolatedComponents.size(); ++i) {
            const auto& comp = isolation.isolatedComponents[i];
            report << "Component " << i + 1 << ":\n";
            report << "  Nodes: " << comp.nodes.size() << "\n";
            report << "  Resistors: " << comp.resistors.size() << "\n";
            report << "  Vias: " << comp.vias.size() << "\n";
            report << "  Tsvs: " << comp.tsvs.size() << "\n";
            report << "  Package Resistors: " << comp.pkgs.size() << "\n";
            report << "  Current Sources: " << comp.isrcs.size() << "\n";

            report << "  Node names (0-9): \n";
            for (size_t i = 0; i < comp.nodes.size() && i < 10; ++i) {
                report << "    " << comp.nodes[i].c_str() << "\n";
            }
            report << "\n";
        }
    }

    // Report open circuits
    if (!openNodes.empty()) {
        report << "=== OPEN CIRCUITS ===\n";
        report << "Nodes with no connections: " << openNodes.size() << "\n";
        for (const auto& node : openNodes) {
            report << "  - Node: " << node.str() << "\n";
        }
        report << "\n";
    }

    // Report short circuits
    if (!shorts.empty()) {
        report << "=== SHORT CIRCUITS ===\n";
        report << "Zero-resistance paths: " << shorts.size() << "\n";
        for (const auto& shortInfo : shorts) {
            report << "  - " << shortInfo.componentName.c_str() << " ("
                   << shortInfo.componentType << ") between "
                   << shortInfo.node1.c_str() << " and "
                   << shortInfo.node2.c_str() << "\n";
        }
    }

    if (isolation.isolatedComponents.empty() && openNodes.empty() &&
        shorts.empty()) {
        report << "No connectivity issues found.\n";
    }

    return report.str();
}
} // namespace pdnsol
