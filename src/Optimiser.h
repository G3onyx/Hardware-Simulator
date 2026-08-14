#pragma once
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <format>
#include "AIG.h"
#include "CompilerGraph.h"
#include "Blueprint.h"
#include "Utils.h"

/// Groups identical, parallel gate-level instructions into 64-bit C++ bitwise operations.
class Vectorizer {
    struct VectorBundle {
        std::vector<size_t> nodes;
        int64_t out_step = 0;
        int64_t in0_step = 0;
        int64_t in1_step = 0;
    };

    std::vector<bool> is_vectorized;
    std::vector<VectorBundle> bundles_at;

    /// Scans ahead in the graph to find the longest valid chain of identical, parallel instructions.
    static VectorBundle find_optimal_vector_bundle(const CompilerGraph& graph, const size_t root_node_idx) {
        const CompNode& root_node = graph.nodes[root_node_idx];
        VectorBundle best_bundle;

        // Sweep lookahead distances to test different structural strides in the netlist
        for (size_t lookahead = 1; lookahead <= 16; ++lookahead) {
            const size_t target_idx = root_node_idx + lookahead;

            if (target_idx >= graph.nodes.size() || graph.nodes[target_idx].is_dead || graph.nodes[target_idx].type != root_node.type) continue;
            if (graph.nodes[target_idx].outputs.size() != 1 || graph.nodes[target_idx].inputs.size() != root_node.inputs.size()) continue;

            // Calculate the expected stride sequence for this candidate bundle
            const int64_t out_step = static_cast<int64_t>(graph.nodes[target_idx].outputs[0]) - static_cast<int64_t>(root_node.outputs[0]);
            const int64_t in0_step = static_cast<int64_t>(graph.nodes[target_idx].inputs[0]) - static_cast<int64_t>(root_node.inputs[0]);
            int64_t in1_step = 0;

            if (root_node.type == PrimitiveType::AIG_AND) {
                in1_step = static_cast<int64_t>(graph.nodes[target_idx].inputs[1]) - static_cast<int64_t>(root_node.inputs[1]);
                // Arguments hold AIG inversion properties; they must match exactly to vectorize safely
                if (graph.nodes[target_idx].args[0] != root_node.args[0] || graph.nodes[target_idx].args[1] != root_node.args[1]) continue;
            }

            if (out_step == 0) continue; // Safety check: prevent overwriting the same net repeatedly

            std::vector candidate_nodes = {root_node_idx};
            int64_t current_out = root_node.outputs[0];
            int64_t current_in0 = root_node.inputs[0];
            int64_t current_in1 = (root_node.type == PrimitiveType::AIG_AND) ? root_node.inputs[1] : 0;

            std::unordered_set<NetID> skipped_outputs;
            std::unordered_set bundled_outputs = { static_cast<NetID>(current_out) };

            // Hazard Tracking: Record nodes bypassed by the initial lookahead leap
            for (size_t prev = root_node_idx + 1; prev < target_idx; ++prev) {
                if (!graph.nodes[prev].is_dead) {
                    for (NetID out_net : graph.nodes[prev].outputs) skipped_outputs.insert(out_net);
                }
            }

            // Gather compatible instructions, halting at the physical 64-bit integer limit
            for (size_t k = target_idx; k < graph.nodes.size() && (k - root_node_idx) < 1000; ++k) {
                if (graph.nodes[k].is_dead) continue;

                if (graph.nodes[k].type == root_node.type &&
                    graph.nodes[k].outputs.size() == 1 &&
                    graph.nodes[k].inputs.size() == root_node.inputs.size() &&
                    graph.nodes[k].outputs[0] == current_out + out_step &&
                    graph.nodes[k].inputs[0] == current_in0 + in0_step) {

                    bool structure_match = true;
                    if (root_node.type == PrimitiveType::AIG_AND) {
                        if (graph.nodes[k].inputs[1] != current_in1 + in1_step ||
                            graph.nodes[k].args[0] != root_node.args[0] ||
                            graph.nodes[k].args[1] != root_node.args[1]) structure_match = false;
                    }

                    if (structure_match) {
                        bool hazard_detected = false;
                        for (NetID in_net : graph.nodes[k].inputs) {
                            if (skipped_outputs.contains(in_net) || bundled_outputs.contains(in_net)) {
                                hazard_detected = true; break;
                            }
                        }

                        if (!hazard_detected) {
                            candidate_nodes.push_back(k);
                            current_out = graph.nodes[k].outputs[0];
                            current_in0 = graph.nodes[k].inputs[0];
                            if (root_node.type == PrimitiveType::AIG_AND) current_in1 = graph.nodes[k].inputs[1];
                            bundled_outputs.insert(current_out);

                            // Prevent C++ bit-shift overflow (UB) on uint64_t types
                            if (candidate_nodes.size() >= Constants::MAX_BUS_WIDTH) break;

                            continue; // Successfully grouped; move to evaluate the next lookahead iteration
                        }
                    }
                    }

                // If rejected, the node becomes a topological hazard barrier for future vector candidates
                for (NetID out_net : graph.nodes[k].outputs) skipped_outputs.insert(out_net);
            }

            if (candidate_nodes.size() > best_bundle.nodes.size())
                best_bundle = {candidate_nodes, out_step, in0_step, in1_step};
        }
        return best_bundle;
    }

    /// Translates a VectorBundle into optimized C++ template calls for read_bus and write_bus.
    static void emit_vectorized_instruction(std::ostream& out, const CompNode& root_node, const VectorBundle& best) {
        const size_t width = best.nodes.size();

        if (root_node.type == PrimitiveType::ROUTE) {
            out << std::format("        write_bus<{}, {}>({}, read_bus<{}, {}>({}));\n", width, best.out_step, root_node.outputs[0], width, best.in0_step, root_node.inputs[0]);
        } else if (root_node.type == PrimitiveType::AIG_NOT) {
            out << std::format("        write_bus<{}, {}>({}, ~(read_bus<{}, {}>({})));\n", width, best.out_step, root_node.outputs[0], width, best.in0_step, root_node.inputs[0]);
        } else if (root_node.type == PrimitiveType::AIG_AND) {
            std::string in0_expr = std::format("read_bus<{}, {}>({})", width, best.in0_step, root_node.inputs[0]);
            std::string in1_expr = std::format("read_bus<{}, {}>({})", width, best.in1_step, root_node.inputs[1]);

            // Apply AIG constant folding inversions dynamically
            if (root_node.args[0]) in0_expr = std::format("(~{})", in0_expr);
            if (root_node.args[1]) in1_expr = std::format("(~{})", in1_expr);

            out << std::format("        write_bus<{}, {}>({}, {} & {});\n", width, best.out_step, root_node.outputs[0], in0_expr, in1_expr);
        }
    }

public:
    explicit Vectorizer(const CompilerGraph& graph) : is_vectorized(graph.nodes.size(), false), bundles_at(graph.nodes.size()) {
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].is_dead || is_vectorized[i]) continue;
            if (graph.nodes[i].type != PrimitiveType::ROUTE && graph.nodes[i].type != PrimitiveType::AIG_AND && graph.nodes[i].type != PrimitiveType::AIG_NOT) continue;
            if (graph.nodes[i].outputs.size() != 1 || graph.nodes[i].inputs.empty()) continue;

            const VectorBundle bundle = find_optimal_vector_bundle(graph, i);
            if (bundle.nodes.size() >= 3) {
                bundles_at[i] = bundle;
                for (const size_t idx : bundle.nodes) is_vectorized[idx] = true;
            }
        }
    }

    /// Evaluates if the current node belongs to a bundle.
    /// If it is the root of the bundle, it emits the optimized C++ and returns true.
    /// If it is skipped data inside a bundle, it suppresses scalar emission and returns true.
    /// Returns false if the transpiler should emit a standard scalar operation.
    bool handle_node(std::ostream& out, const size_t node_idx, const CompNode& node) const {
        if (!bundles_at[node_idx].nodes.empty()) {
            emit_vectorized_instruction(out, node, bundles_at[node_idx]);
            return true;
        }
        return is_vectorized[node_idx];
    }

    [[nodiscard]] bool is_node_vectorized(const size_t node_idx) const {
        return is_vectorized[node_idx];
    }
};

class Optimiser {
    /// Translates the raw gate-level netlist into a structurally hashed AND-Inverter Graph (AIG)
    class AIGGenerator {
        AIG& aig;
        const CompilerGraph& graph;
        std::vector<std::optional<AIGEdge>>& bit_map;
        std::unordered_map<uint64_t, AIGEdge> strash_table;
        std::vector<const CompNode*> net_to_node;

        /// Adds an AND node to the AIG, applying structural hashing and basic constant folding
        AIGEdge add_and(AIGEdge left, AIGEdge right) {
            if (left.data == 0 || right.data == 0) return AIGEdge::make(0, false);
            if (left.data == 1) return right;
            if (right.data == 1) return left;
            if (left.data == right.data) return left;
            if ((left.data ^ 1) == right.data) return AIGEdge::make(0, false);

            if (left.data > right.data) std::swap(left, right);

            const uint64_t hash_key = (static_cast<uint64_t>(left.data) << 32) | right.data;
            if (strash_table.contains(hash_key)) return strash_table[hash_key];

            const uint32_t id = aig.nodes.size();
            const uint32_t left_depth = left.get_node() < aig.nodes.size() ? aig.nodes[left.get_node()].depth : 0;
            const uint32_t right_depth = right.get_node() < aig.nodes.size() ? aig.nodes[right.get_node()].depth : 0;
            const uint32_t depth = std::max(left_depth, right_depth) + 1;

            aig.nodes.push_back({left, right, 0, static_cast<NetID>(-1), depth});

            const AIGEdge new_edge = AIGEdge::make(id, false);
            strash_table[hash_key] = new_edge;
            return new_edge;
        }

        /// Recursively evaluates a net on-demand.
        /// This Depth-First Search acts as a topological sort, ensuring that out-of-order HDL declarations do not cause undefined reads in the AIG.
        AIGEdge resolve_net_recursively(const NetID net) {
            // Base case: If the net is already resolved, return it
            if (bit_map[net].has_value()) return bit_map[net].value();

            const CompNode* node = net_to_node[net];
            if (!node) return AIGEdge::make(0, false); // Floating or disconnected pin defaults to 0

            if (node->type == PrimitiveType::NAND) {
                const AIGEdge a = resolve_net_recursively(node->inputs[0]);
                const AIGEdge b = resolve_net_recursively(node->inputs[1]);
                const AIGEdge and_edge = add_and(a, b);

                // NAND logic is represented as an inverted AND edge
                AIGEdge res = AIGEdge::make(and_edge.get_node(), !and_edge.is_inverted());
                bit_map[net] = res;
                return res;

            }
            if (node->type == PrimitiveType::ROUTE) {
                for (size_t i = 0; i < node->outputs.size(); ++i) {
                    if (node->outputs[i] == net) {
                        AIGEdge res = resolve_net_recursively(node->inputs[i]);
                        bit_map[net] = res;
                        return res;
                    }
                }
            }
            return AIGEdge::make(0, false);
        }

    public:
        AIGGenerator(AIG& aig_ref, const CompilerGraph& graph_ref, std::vector<std::optional<AIGEdge>>& bit_map_ref)
            : aig(aig_ref), graph(graph_ref), bit_map(bit_map_ref) {}

        /// Establishes the graph's fundamental inputs (constants, IO, memory read ports)
        void initialize_leaf_nodes(const Blueprint& top_bp) const {
            aig.nodes.push_back({AIGEdge::make(0), AIGEdge::make(0), 0, NET_FALSE});
            bit_map[NET_FALSE] = AIGEdge::make(0, false);
            bit_map[NET_TRUE]  = AIGEdge::make(0, true);

            // Pre-map the physical Top-Level Input pins
            for (const auto& [name, nets] : graph.io_mapping) {
                if (std::ranges::find(top_bp.in_wires, name) != top_bp.in_wires.end()) {
                    for (const NetID net : nets) {
                        const uint32_t id = aig.nodes.size();
                        aig.nodes.push_back({AIGEdge::make(0), AIGEdge::make(0), 0, net});
                        bit_map[net] = AIGEdge::make(id, false);
                    }
                }
            }

            // Pre-map Sequential/Macro outputs globally so they behave as root inputs to combinational logic
            for (const auto& node : graph.nodes) {
                if (node.is_dead || node.type == PrimitiveType::NAND || node.type == PrimitiveType::ROUTE) continue;
                for (const NetID out_net : node.outputs) {
                    if (!bit_map[out_net].has_value()) {
                        const uint32_t id = aig.nodes.size();
                        aig.nodes.push_back({AIGEdge::make(0), AIGEdge::make(0), 0, out_net});
                        bit_map[out_net] = AIGEdge::make(id, false);
                    }
                }
            }
        }

        /// Sweeps through the graph and constructs the pure AIG mathematically
        void build_graph() {
            net_to_node.assign(graph.net_count, nullptr);

            // Build a lookup table to answer "Which node drives this NetID?"
            for (const auto& node : graph.nodes) {
                if (node.is_dead) continue;
                if (node.type == PrimitiveType::NAND || node.type == PrimitiveType::ROUTE) {
                    for (const NetID out_net : node.outputs) net_to_node[out_net] = &node;
                }
            }

            // Perform Topological DFS to resolve all paths safely
            for (const auto& node : graph.nodes) {
                if (node.is_dead) continue;
                if (node.type == PrimitiveType::NAND || node.type == PrimitiveType::ROUTE) {
                    for (const NetID out_net : node.outputs) resolve_net_recursively(out_net);
                }
            }
        }
    };

    /// Strips away dead logic and emits the optimized AIG back into the CompilerGraph
    class AIGEmitter {
        /// Finds physical nets required to drive state/macro hardware (DFFs, RAM, Output pins)
        static std::set<NetID> identify_required_sinks(const CompilerGraph& graph, const Blueprint& top_bp) {
            std::set<NetID> sink_nets;
            for (const auto& node : graph.nodes) {
                if (node.is_dead || node.type == PrimitiveType::NAND) continue;
                for (NetID in_net : node.inputs) sink_nets.insert(in_net);
            }
            for (const auto& [name, nets] : graph.io_mapping) {
                if (std::ranges::find(top_bp.out_wires, name) != top_bp.out_wires.end()) {
                    for (NetID n : nets) sink_nets.insert(n);
                }
            }
            return sink_nets;
        }

        /// Marks the required sink targets as 'alive' inside the mathematical AIG
        static void mark_sink_roots_alive(AIG& aig, const std::set<NetID>& sinks, const std::vector<std::optional<AIGEdge>>& bit_map) {
            for (const NetID sink : sinks) {
                if (bit_map[sink].has_value()) aig.nodes[bit_map[sink]->get_node()].ref_count++;
            }
        }

        /// Propagates liveness downward to strip away Dead Code (DCE)
        static void execute_reverse_liveness_sweep(AIG& aig) {
            for (size_t i = aig.nodes.size() - 1; i > 0; --i) {
                if (aig.nodes[i].ref_count > 0) {
                    if (aig.nodes[i].left.data != 0 || aig.nodes[i].right.data != 0) {
                        aig.nodes[aig.nodes[i].left.get_node()].ref_count++;
                        aig.nodes[aig.nodes[i].right.get_node()].ref_count++;
                    }
                }
            }
        }

        /// Translates live AIG nodes sequentially back into physical CompilerGraph nets
        static std::vector<NetID> allocate_and_emit(const AIG& aig, CompilerGraph& graph) {
            std::vector aig_to_net(aig.nodes.size(), static_cast<NetID>(-1));
            aig_to_net[0] = NET_FALSE;

            std::vector<uint32_t> compute_nodes;
            for (size_t i = 1; i < aig.nodes.size(); ++i) {
                auto& node = aig.nodes[i];
                if (node.ref_count == 0) continue;
                if (node.left.data == 0 && node.right.data == 0) aig_to_net[i] = node.origin_net;
                else compute_nodes.push_back(i);
            }

            // Levelized compilation enables CPU superscalar ILP by forcing instructions with independent topological depths to execute consecutively
            std::ranges::stable_sort(compute_nodes, [&aig](const uint32_t a, const uint32_t b) {
                return aig.nodes[a].depth < aig.nodes[b].depth;
            });

            for (const uint32_t i : compute_nodes) {
                auto& node = aig.nodes[i];
                NetID target_net = graph.add_net();
                aig_to_net[i] = target_net;

                NetID left_net = aig_to_net[node.left.get_node()];
                NetID right_net = aig_to_net[node.right.get_node()];

                graph.add_node(PrimitiveType::AIG_AND, {left_net, right_net}, {target_net},
                               {node.left.is_inverted(), node.right.is_inverted()}, "AIG_AND");
            }
            return aig_to_net;
        }

        /// Reconnects the newly compiled logic directly to the hardware sinks
        static void connect_sinks(CompilerGraph& graph, const std::set<NetID>& sinks, const std::vector<std::optional<AIGEdge>>& bit_map, const std::vector<NetID>& aig_to_net) {
            for (NetID target_net : sinks) {
                if (bit_map[target_net].has_value()) {
                    const AIGEdge edge = bit_map[target_net].value();
                    if (edge.get_node() == 0) {
                        graph.add_node(PrimitiveType::ROUTE, {edge.is_inverted() ? NET_TRUE : NET_FALSE}, {target_net}, {}, "ROUTE");
                    } else if (edge.is_inverted()) {
                        graph.add_node(PrimitiveType::AIG_NOT, {aig_to_net[edge.get_node()]}, {target_net}, {}, "AIG_NOT");
                    } else if (aig_to_net[edge.get_node()] != target_net) {
                        graph.add_node(PrimitiveType::ROUTE, {aig_to_net[edge.get_node()]}, {target_net}, {}, "ROUTE");
                    }
                }
            }
        }

    public:
        static void execute(AIG& aig, CompilerGraph& graph, const std::vector<std::optional<AIGEdge>>& bit_map, const Blueprint& top_bp) {
            for (auto& node : aig.nodes) node.ref_count = 0;
            const auto sinks = identify_required_sinks(graph, top_bp);

            mark_sink_roots_alive(aig, sinks, bit_map);
            execute_reverse_liveness_sweep(aig);
            const auto aig_to_net = allocate_and_emit(aig, graph);

            connect_sinks(graph, sinks, bit_map, aig_to_net);
        }
    };

    /// Shrinks the physical state[] array by forcing all localized logic to use transient NetIDs
    static void minimize_state_elements(CompilerGraph &graph) {
        std::vector array_bound(graph.net_count, false);

        // Core global memory hooks
        array_bound[NET_FALSE] = true;
        array_bound[NET_TRUE] = true;
        for (const auto& nets : graph.io_mapping | std::views::values) {
            for (const NetID n : nets) array_bound[n] = true;
        }

        // Hardware state dependencies (RAM, ROM, DFF, Peripherals)
        for (const auto& node : graph.nodes) {
            if (node.is_dead) continue;
            if (node.type != PrimitiveType::ROUTE && node.type != PrimitiveType::AIG_AND && node.type != PrimitiveType::AIG_NOT) {
                for (const NetID input : node.inputs) array_bound[input] = true;
                for (const NetID output : node.outputs) array_bound[output] = true;
            }
        }

        // Prevent fast vectorised bundles from ripping themselves out of the bus
        const Vectorizer vec(graph);
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (vec.is_node_vectorized(i)) {
                for (const NetID input : graph.nodes[i].inputs) array_bound[input] = true;
                for (const NetID output : graph.nodes[i].outputs) array_bound[output] = true;
            }
        }

        // Remap the graph cleanly
        std::vector<NetID> net_remap(graph.net_count, 0);
        uint32_t state_idx = 2; // [0, 1] are reserved

        uint32_t total_state_nets = 2;
        for (uint32_t i = 2; i < graph.net_count; ++i) {
            if (array_bound[i]) total_state_nets++;
        }

        uint32_t temp_idx = total_state_nets;
        net_remap[NET_FALSE] = NET_FALSE;
        net_remap[NET_TRUE] = NET_TRUE;

        // Force 'array_bound' variables to the bottom of the stack, and local 'const uint8_t n_X' logic upwards
        for (uint32_t i = 2; i < graph.net_count; ++i) {
            if (array_bound[i]) net_remap[i] = state_idx++;
            else net_remap[i] = temp_idx++;
        }

        for (auto& node : graph.nodes) {
            if (node.is_dead) continue;
            for (auto& in : node.inputs) in = net_remap[in];
            for (auto& out : node.outputs) out = net_remap[out];
        }
        for (auto& nets : graph.io_mapping | std::views::values) {
            for (auto& n : nets) n = net_remap[n];
        }

        // Truncate memory allocation ceiling natively before the transpiler runs
        graph.net_count = total_state_nets;
    }

public:
    /// Primary entrance for deep structural logic optimization.
    /// Replaces the raw unoptimized NAND netlist with a heavily compressed and routed AIG.
    static void optimise_graph(CompilerGraph &graph, const Blueprint &top_bp) {
        std::vector<std::optional<AIGEdge>> bit_map(graph.net_count, std::nullopt);
        AIG aig;

        AIGGenerator generator(aig, graph, bit_map);
        generator.initialize_leaf_nodes(top_bp);
        generator.build_graph();

        // Tombstone the original logic before compacting and emitting the optimized logic
        for (auto& node : graph.nodes) {
            if (node.type == PrimitiveType::NAND || node.type == PrimitiveType::ROUTE) node.is_dead = true;
        }
        graph.compact_graph();

        AIGEmitter::execute(aig, graph, bit_map, top_bp);

        graph.resolve_aliases();
        graph.compact_nets();
        graph.sort_nodes();

        minimize_state_elements(graph);
    }
};