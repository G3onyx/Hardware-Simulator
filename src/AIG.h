#pragma once
#include <vector>
#include <iostream>
#include <string>
#include <cstdint>

struct AIGEdge {
    uint32_t data;

    [[nodiscard]] bool is_inverted() const { return data & 1; }
    [[nodiscard]] uint32_t get_node() const { return data >> 1; }

    static AIGEdge make(const uint32_t node, const bool invert = false) {
        return {(node << 1) | (invert ? 1 : 0)};
    }

    bool operator==(const AIGEdge &other) const { return data == other.data; }
};

struct AIGNode {
    AIGEdge left{}, right{};
    uint32_t ref_count = 0; // Tracks how many dependent nodes need this result
    NetID origin_net = static_cast<NetID>(-1); // Maps leaf nodes back to their real graph pins
    uint32_t depth;
};

struct AIG {
    std::vector<AIGNode> nodes;

    void print_debug(const bool verbose = false) const {
        std::cout << "\n=== AIG Debug ===\n";
        std::cout << "Total Nodes: " << nodes.size() << "\n";

        if (verbose && !nodes.empty()) {
            std::cout << "\n--- Node Details ---\n";
            std::cout << "  [0] Const 0 | Refs: " << nodes[0].ref_count << " | Depth: " << nodes[0].depth << "\n";

            for (size_t i = 1; i < nodes.size(); ++i) {
                const auto& node = nodes[i];
                std::cout << "  [" << i << "] ";

                // A node pointing entirely to Constant 0 represents a Primary Input/Leaf Node
                if (node.left.data == 0 && node.right.data == 0) {
                    std::cout << "Primary Input ";
                    if (node.origin_net != static_cast<NetID>(-1)) std::cout << "(Net: " << node.origin_net << ")";
                    else std::cout << "(Unmapped)";
                } else {
                    auto format_edge = [](const AIGEdge& e) {
                        return (e.is_inverted() ? "~" : "") + std::to_string(e.get_node());
                    };
                    std::cout << "AND(" << format_edge(node.left) << ", " << format_edge(node.right) << ")";
                }

                std::cout << " | Refs: " << node.ref_count << " | Depth: " << node.depth << "\n";
            }
        }
        std::cout << "=================\n\n";
    }
};