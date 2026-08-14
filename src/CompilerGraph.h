#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <ranges>

using NetID = uint32_t;
using NodeID = uint32_t;

constexpr NetID NET_FALSE = 0;
constexpr NetID NET_TRUE = 1;

enum class PrimitiveType : uint8_t {
    NAND, DFF, RAM, ROM, // True primitives
    ROUTE, PERIPHERAL,
    AIG_AND, AIG_NOT,
    DEAD,

    // Structural macro categories
    MACRO_MATH,    // out = a OP b (width = a.width)
    MACRO_COMPARE, // out = a OP b (width = 1)
    MACRO_UNARY,   // out = OP a
    MACRO_MUX,     // out = sel ? b : a
    MACRO_DMUX     // a = sel ? 0 : in, b = sel ? in : 0
};

/// Represents a logic gate, macro, or generalized C++ operator during compilation
struct CompNode {
    PrimitiveType type;
    std::string chip_type, debug_name;
    bool is_dead = false; // For tombstoning
    std::vector<NetID> inputs, outputs;
    std::vector<uint32_t> args; // Used for bit-widths, memory sizes, etc.
    std::string tag;
};

class CompilerGraph {
public:
    uint32_t net_count = 2;
    std::vector<CompNode> nodes;

    // Only stores strings for the absolute top-level pins so the Host C++ API can find them
    std::unordered_map<std::string, std::vector<NetID>> io_mapping;

    NetID add_net() { return net_count++; }

    NodeID add_node(const PrimitiveType type,
                    const std::vector<NetID> &inputs = {},
                    const std::vector<NetID> &outputs = {},
                    const std::vector<uint32_t> &args = {},
                    const std::string &chip_type = "",
                    const std::string &debug_name = "",
                    const std::string &tag = "") {
        const NodeID id = nodes.size();
        nodes.push_back({type, chip_type, debug_name, false, inputs, outputs, args, tag});
        return id;
    }

    void remove_node(const NodeID id) {
        if (id < nodes.size()) {
            nodes[id].is_dead = true;
            nodes[id].inputs.clear();
            nodes[id].outputs.clear();
        }
    }

    /// Cleans up tombstones and rebuilds a dense graph
    void compact_graph() {
        std::vector<CompNode> active_nodes;
        for (auto &node: nodes) {
            if (!node.is_dead) active_nodes.push_back(std::move(node));
        }
        nodes = std::move(active_nodes);
    }

    /// Resolves structural aliases created by sub-chip boundaries and explicit ROUTE nodes.
    /// Safely repoints all consumers of a ROUTE node to read directly from the true source driver.
    void resolve_aliases() {
        std::vector<NetID> driver_table(net_count);
        for (size_t i = 0; i < driver_table.size(); ++i) driver_table[i] = i;

        // Map all outputs of a ROUTE node back to its inputs
        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& node = nodes[i];
            if (node.is_dead || node.type != PrimitiveType::ROUTE) continue;

            if (node.inputs.size() != node.outputs.size())
                throw std::runtime_error("ROUTE node has mismatched input/output total widths.");

            for (size_t j = 0; j < node.outputs.size(); ++j) {
                driver_table[node.outputs[j]] = node.inputs[j];
            }

            remove_node(i);
        }

        // Trace pointer chains to their absolute root (e.g. A -> B -> C becomes A -> C)
        auto get_true_source = [&](NetID id) -> NetID {
            int depth = 0;
            while (driver_table[id] != id) {
                id = driver_table[id];
                if (depth++ > 1000) throw std::runtime_error("Circular routing detected.");
            }
            return id;
        };

        // Update all surviving nodes with their resolved true pointers
        for (auto& node : nodes) {
            if (node.is_dead) continue;
            for (auto& in : node.inputs) in = get_true_source(in);
            for (auto& out : node.outputs) out = get_true_source(out);
        }

        // Update the top-level IO mapping so Host APIs access the correct physical nets
        for (auto &nets: io_mapping | std::views::values) {
            for (auto& net : nets) net = get_true_source(net);
        }
    }

    /// Reallocates NetIDs to remove unused "holes" in the state array, maximizing CPU cache locality.
    void compact_nets() {
        std::vector<NetID> net_remap(net_count, 0);
        std::vector net_used(net_count, false);

        // Nets 0 and 1 are hardcoded hardware constants
        net_used[NET_FALSE] = true;
        net_used[NET_TRUE] = true;
        net_remap[NET_FALSE] = NET_FALSE;
        net_remap[NET_TRUE] = NET_TRUE;

        // Mark all nets referenced by active nodes or IO mappings
        for (const auto& node : nodes) {
            if (node.is_dead) continue;
            for (const NetID in : node.inputs) net_used[in] = true;
            for (const NetID out : node.outputs) net_used[out] = true;
        }
        for (const auto& nets : io_mapping | std::views::values) {
            for (const NetID n : nets) net_used[n] = true;
        }

        // Build the contiguous mapping
        uint32_t new_net_count = 2;
        for (uint32_t i = 2; i < net_count; ++i) {
            if (net_used[i]) net_remap[i] = new_net_count++;
        }

        // Apply the mapping to shift all pointers down
        for (auto& node : nodes) {
            if (node.is_dead) continue;
            for (auto& in : node.inputs) in = net_remap[in];
            for (auto& out : node.outputs) out = net_remap[out];
        }
        for (auto& nets : io_mapping | std::views::values) {
            for (auto& n : nets) n = net_remap[n];
        }

        net_count = new_net_count; // Shrink the array
    }

    void sort_nodes() {
        std::vector<uint32_t> in_degrees(nodes.size(), 0);
        std::vector<std::vector<uint32_t>> adj(nodes.size());
        std::vector driver(net_count, static_cast<uint32_t>(-1));

        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].is_dead || nodes[i].type == PrimitiveType::DFF || nodes[i].type == PrimitiveType::PERIPHERAL) continue;
            for (const NetID out : nodes[i].outputs) driver[out] = i;
        }

        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].is_dead || nodes[i].type == PrimitiveType::DFF || nodes[i].type == PrimitiveType::PERIPHERAL) continue;
            std::vector<uint32_t> deps;

            size_t start_in = 0;
            size_t end_in = nodes[i].inputs.size();

            // Break false combinational cycles: RAM outputs only depend combinationally on the address
            if (nodes[i].type == PrimitiveType::RAM) {
                const uint32_t data_w = nodes[i].args[1];
                const uint32_t addr_w = nodes[i].args[0];
                start_in = data_w;
                end_in = data_w + addr_w;
            }

            for (size_t j = start_in; j < end_in; ++j) {
                uint32_t d = driver[nodes[i].inputs[j]];
                if (d != static_cast<uint32_t>(-1) && d != i) deps.push_back(d);
            }
            std::ranges::sort(deps);
            deps.erase(std::ranges::unique(deps).begin(), deps.end());

            for (const uint32_t d : deps) {
                adj[d].push_back(i);
                in_degrees[i]++;
            }
        }

        std::vector<CompNode> sorted;
        sorted.reserve(nodes.size());
        std::vector<uint32_t> queue;

        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].is_dead || nodes[i].type == PrimitiveType::DFF || nodes[i].type == PrimitiveType::PERIPHERAL)
                sorted.push_back(std::move(nodes[i]));
            else if (in_degrees[i] == 0)
                queue.push_back(i);
        }

        size_t head = 0;
        while (head < queue.size()) {
            uint32_t u = queue[head++];
            sorted.push_back(std::move(nodes[u]));
            for (uint32_t v : adj[u]) {
                if (--in_degrees[v] == 0) queue.push_back(v);
            }
        }

        // Catch cyclic logic to prevent node deletion (even if it's a hardware error)
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!(nodes[i].is_dead || nodes[i].type == PrimitiveType::DFF || nodes[i].type == PrimitiveType::PERIPHERAL) && in_degrees[i] > 0)
                sorted.push_back(std::move(nodes[i]));
        }

        nodes = std::move(sorted);
    }

    void print_debug(const bool verbose = false) const {
        std::cout << "=== CompilerGraph Debug ===\n";

        uint32_t active_nodes = 0;
        for (const auto& node : nodes) {
            if (!node.is_dead) active_nodes++;
        }

        std::cout << "Total Nets:  " << net_count << "\n";
        std::cout << "Total Nodes: " << active_nodes << " (Allocated: " << nodes.size() << ")\n";

        if (verbose) {
            auto type_to_string = [](const PrimitiveType pt) {
                switch(pt) {
                    case PrimitiveType::NAND: return "NAND";
                    case PrimitiveType::DFF: return "DFF";
                    case PrimitiveType::RAM: return "RAM";
                    case PrimitiveType::ROM: return "ROM";
                    case PrimitiveType::ROUTE: return "ROUTE";
                    case PrimitiveType::PERIPHERAL: return "PERIPHERAL";
                    case PrimitiveType::AIG_AND: return "AIG_AND";
                    case PrimitiveType::AIG_NOT: return "AIG_NOT";
                    case PrimitiveType::DEAD: return "DEAD";
                    default: return "UNKNOWN";
                }
            };

            std::cout << "\n--- Node Details ---\n";
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].is_dead) continue; // Skip tombstones

                std::cout << "  [" << i << "] " << type_to_string(nodes[i].type);
                if (!nodes[i].chip_type.empty()) std::cout << " (" << nodes[i].chip_type << ")";
                if (!nodes[i].debug_name.empty()) std::cout << " '" << nodes[i].debug_name << "'";

                std::cout << "\n    In:  ";
                for (const auto in : nodes[i].inputs) std::cout << in << " ";

                std::cout << "\n    Out: ";
                for (const auto out : nodes[i].outputs) std::cout << out << " ";

                if (!nodes[i].args.empty()) {
                    std::cout << "\n    Args: ";
                    for (auto a : nodes[i].args) std::cout << a << " ";
                }
                std::cout << "\n";
            }

            std::cout << "\n--- IO Mapping ---\n";
            for (const auto& [name, nets] : io_mapping) {
                std::cout << "  " << name << ": ";
                for (const auto n : nets) std::cout << n << " ";
                std::cout << "\n";
            }
        }
        std::cout << "===========================\n\n";
    }
};
