//
// Created by georg on 17/07/2026.
//

#pragma once
#include "HDLLexer.h"
#include "HDLParser.h"
#include "Blueprint.h"
#include "HDLToBlueprint.h"
#include "CompilerGraph.h"
#include "Utils.h"
#include <ranges>
#include <unordered_set>
#include <fstream>
#include <variant>

struct Scope {
    std::string path;
    std::unordered_map<std::string, NetID> nets;      // Stores the first bit (base ID) of the wire
    std::unordered_map<std::string, uint32_t> widths; // Stores total bits
    std::unordered_map<std::string, uint64_t> values;
};

class Builder {
    std::unordered_map<std::string, Blueprint> library;
    std::vector<std::string> loading_stack; // used to detect circular dependencies

    void inject_builtins() {
        // Helper for Math/Logic macros (out[WIDTH] = a[WIDTH] OP b[WIDTH])
        auto make_math = [&](const std::string& name) {
            Blueprint bp;
            bp.name = name;
            bp.generic_params = {"WIDTH"};
            bp.flags["macro"].emplace_back(name);
            bp.wires["a"] = {"a", {"WIDTH"}}; bp.in_wires.emplace_back("a");
            bp.wires["b"] = {"b", {"WIDTH"}}; bp.in_wires.emplace_back("b");
            bp.wires["out"] = {"out", {"WIDTH"}}; bp.out_wires.emplace_back("out");
            library[name] = std::move(bp);
            injected_builtins.insert(name);
        };

        // Helper for Comparison macros (out = a[WIDTH] OP b[WIDTH])
        auto make_compare = [&](const std::string& name) {
            Blueprint bp;
            bp.name = name;
            bp.generic_params = {"WIDTH"};
            bp.flags["macro"].emplace_back(name);
            bp.wires["a"] = {"a", {"WIDTH"}}; bp.in_wires.emplace_back("a");
            bp.wires["b"] = {"b", {"WIDTH"}}; bp.in_wires.emplace_back("b");
            bp.wires["out"] = {"out", {}}; bp.out_wires.emplace_back("out");
            library[name] = std::move(bp);
            injected_builtins.insert(name);
        };

        for (const auto& op : {"Add", "Sub", "Mul", "Div", "And", "Or", "Xor", "Shl", "Shr"}) make_math(op);
        for (const auto& op : {"Eq", "Neq", "Lt", "Lte", "Gt", "Gte"}) make_compare(op);

        // Unary (Not)
        Blueprint not_bp;
        not_bp.name = "Not"; not_bp.generic_params = {"WIDTH"}; not_bp.flags["macro"].emplace_back("Not");
        not_bp.wires["in"] = {"in", {"WIDTH"}}; not_bp.in_wires.emplace_back("in");
        not_bp.wires["out"] = {"out", {"WIDTH"}}; not_bp.out_wires.emplace_back("out");
        library["Not"] = std::move(not_bp);
        injected_builtins.insert("Not");

        // Mux
        Blueprint mux_bp;
        mux_bp.name = "Mux"; mux_bp.generic_params = {"WIDTH"}; mux_bp.flags["macro"].emplace_back("Mux");
        mux_bp.wires["a"] = {"a", {"WIDTH"}}; mux_bp.in_wires.emplace_back("a");
        mux_bp.wires["b"] = {"b", {"WIDTH"}}; mux_bp.in_wires.emplace_back("b");
        mux_bp.wires["sel"] = {"sel", {}}; mux_bp.in_wires.emplace_back("sel");
        mux_bp.wires["out"] = {"out", {"WIDTH"}}; mux_bp.out_wires.emplace_back("out");
        library["Mux"] = std::move(mux_bp);
        injected_builtins.insert("Mux");

        // DMux
        Blueprint dmux_bp;
        dmux_bp.name = "DMux"; dmux_bp.generic_params = {"WIDTH"}; dmux_bp.flags["macro"].emplace_back("DMux");
        dmux_bp.wires["in"] = {"in", {"WIDTH"}}; dmux_bp.in_wires.emplace_back("in");
        dmux_bp.wires["sel"] = {"sel", {}}; dmux_bp.in_wires.emplace_back("sel");
        dmux_bp.wires["a"] = {"a", {"WIDTH"}}; dmux_bp.out_wires.emplace_back("a");
        dmux_bp.wires["b"] = {"b", {"WIDTH"}}; dmux_bp.out_wires.emplace_back("b");
        library["DMux"] = std::move(dmux_bp);
        injected_builtins.insert("DMux");
    }

    static Blueprint hdl_to_blueprint(const std::string &filepath, const std::vector<std::string>& search_paths) {
        // Load HDL file
        std::ifstream stream(filepath, std::ios::binary);
        if (!stream.is_open())
            throw std::runtime_error("Could not open file: " + filepath);

        std::string content((std::istreambuf_iterator(stream)), std::istreambuf_iterator<char>());
        stream.close();

        // Set up ANTLR pipeline
        antlr4::ANTLRInputStream input(content);
        HDLLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        HDLParser parser(&tokens);

        // Parse file
        HDLParser::ChipContext *tree = parser.chip();

        // Check for ANTLR syntax errors
        if (parser.getNumberOfSyntaxErrors() > 0) throw std::runtime_error("Syntax errors found in " + filepath);

        // Build the blueprint
        Blueprint bp;
        HDLToBlueprint visitor(bp, search_paths);
        visitor.visit(tree);

        return bp;
    }

    static uint64_t interpret_literal(std::string text) {
        std::erase(text, '_');
        const size_t tick_pos = text.find('\'');
        if (tick_pos == std::string::npos) return std::stoull(text);

        const std::string val_str = text.substr(tick_pos + 1);
        if (val_str.empty()) throw std::invalid_argument("Malformed literal: " + text);

        const char base = val_str[0];
        const std::string digits = val_str.substr(1);

        switch (base) {
            case 'b': return std::stoull(digits, nullptr, 2);
            case 'd': return std::stoull(digits, nullptr, 10);
            case 'x': return std::stoull(digits, nullptr, 16);
            default: throw std::invalid_argument("Unknown literal base in: " + text);
        }
    }

    static uint64_t resolve_value(const std::string &expr, const Scope &scope) {
        if (scope.values.contains(expr)) return scope.values.at(expr);
        return interpret_literal(expr);
    }

    static void resolve_chunk(const Chunk & chunk, const Scope &scope, CompilerGraph &graph, std::vector<NetID> &target) {
        if (std::holds_alternative<Slice>(chunk)) {
            const auto &slice = std::get<Slice>(chunk);
            const uint32_t rep_count = resolve_value(slice.rep_count, scope);

            if (slice.wire_name == "_") {
                for (uint32_t r = 0; r < rep_count; ++r) target.push_back(graph.add_net());
                return;
            }

            if (!scope.nets.contains(slice.wire_name))
                throw std::runtime_error("Net not found in scope: '" + slice.wire_name + "'");

            const NetID base_id = scope.nets.at(slice.wire_name);
            uint16_t left = 0, right = 0;

            if (!slice.lsb.empty()) {
                left = resolve_value(slice.lsb, scope);
                right = slice.msb.empty() ? left : resolve_value(slice.msb, scope);
            } else if (!slice.wire_dims.empty()) {
                left = resolve_value(slice.wire_dims[0], scope);
                right = left;
            } else {
                left = 0;
                right = scope.widths.at(slice.wire_name) - 1;
            }

            for (uint32_t r = 0; r < rep_count; ++r) {
                if (left <= right) {
                    for (int b = left; b <= right; ++b) target.push_back(base_id + b);
                } else {
                    for (int b = left; b >= right; --b) target.push_back(base_id + b);
                }
            }
        } else {
            const auto &[val_str, rep_str] = std::get<Literal>(chunk);
            const uint32_t rep_count = resolve_value(rep_str, scope);
            const uint32_t actual_val = resolve_value(val_str, scope);

            uint32_t lit_width = 1;
            if (const size_t tick_pos = val_str.find('\''); tick_pos != std::string::npos)
                lit_width = std::stoi(val_str.substr(0, tick_pos));

            // Blast literals directly LSB -> MSB
            for (uint32_t r = 0; r < rep_count; ++r) {
                for (int b = 0; b < static_cast<int>(lit_width); ++b)
                    target.push_back(((actual_val >> b) & 1ULL) ? NET_TRUE : NET_FALSE);
            }
        }
    }

    static void resolve_blueprint_nets(const Blueprint &bp, Scope &scope, CompilerGraph &graph) {
        for (const auto &[name, value_str]: bp.constants)
            scope.values[name] = resolve_value(value_str, scope);

        for (const auto &[wire_name, wire]: bp.wires) {
            if (scope.nets.contains(wire_name)) continue;

            uint32_t total_width = 1;
            for (const std::string &dim: wire.dims) total_width *= resolve_value(dim, scope);

            if (total_width > Constants::MAX_BUS_WIDTH)
                throw std::runtime_error("Wire '" + wire_name + "' exceeds " + std::to_string(Constants::MAX_BUS_WIDTH) + "-bit maximum bus width limit (" + std::to_string(total_width) + ").");

            scope.widths[wire_name] = total_width;
            scope.nets[wire_name] = graph.net_count; // Claim the base ID

            for (uint32_t i = 0; i < total_width; ++i) graph.add_net();
        }
    }

    static std::unordered_map<std::string, std::vector<NetID>> resolve_part_pins(
        const std::unordered_map<std::string, Bundle> &pins, const Scope &scope, CompilerGraph &graph) {
        std::unordered_map<std::string, std::vector<NetID>> resolved_pins;
        for (const auto &[pin_name, bundle]: pins) {
            if (!bundle.empty()) {
                for (const auto & it : std::ranges::reverse_view(bundle))
                    resolve_chunk(it, scope, graph, resolved_pins[pin_name]);
            }
        }
        return resolved_pins;
    }

    static void route_subchip_boundaries(const Blueprint &part_bp, const std::unordered_map<std::string, Bundle> &pins,
                                         const Scope &scope, const Scope &child_scope, CompilerGraph &graph,
                                         const std::string &part_path) {
        for (const auto &[pin_name, bundle]: pins) {
            if (bundle.empty()) continue;
            if (!child_scope.nets.contains(pin_name)) throw std::runtime_error("Pin '" + pin_name + "' does not exist on subchip");

            const NetID child_base = child_scope.nets.at(pin_name);
            const uint32_t child_width = child_scope.widths.at(pin_name);

            std::vector<NetID> child_nets;
            for(uint32_t i = 0; i < child_width; ++i) child_nets.push_back(child_base + i);

            std::vector<NetID> parent_nets;
            for (const auto & it : std::ranges::reverse_view(bundle))
                resolve_chunk(it, scope, graph, parent_nets);

            const bool is_input = std::ranges::find(part_bp.in_wires, pin_name) != part_bp.in_wires.end();
            const bool is_output = std::ranges::find(part_bp.out_wires, pin_name) != part_bp.out_wires.end();

            if (is_input)
                graph.add_node(PrimitiveType::ROUTE, parent_nets, child_nets, {}, "__Assign", std::format("{}.__In_{}", part_path, pin_name));
            else if (is_output)
                graph.add_node(PrimitiveType::ROUTE, child_nets, parent_nets, {}, "__Assign", std::format("{}.__Out_{}", part_path, pin_name));
            else throw std::runtime_error("Pin '" + pin_name + "' is not declared as an IO port");
        }
    }

    void expand_into_graph(const std::string &chip_type, Scope &scope, CompilerGraph &graph, const int depth, const std::string& inherited_tag = "") const {
        const Blueprint &bp = library.at(chip_type);
        resolve_blueprint_nets(bp, scope, graph);

        for (size_t i = 0; i < bp.parts.size(); ++i) {
            const auto &[type, generic_args, pins, tag] = bp.parts[i];
            const std::string part_path = std::format("{}.{}_{}", scope.path, type, std::to_string(i));

            // Merge the explicit tag with the inherited tag
            const std::string active_tag = tag.empty() ? inherited_tag : tag;

            auto resolved_pins = resolve_part_pins(pins, scope, graph);

            // Handle implicit routing nodes (__Assign)
            if (internal_primitives.contains(type)) {
                std::vector<NetID> inputs, outputs;
                auto add_pin_to = [&](const std::string &pin_name, std::vector<NetID> &target) {
                    if (resolved_pins.contains(pin_name))
                        target.insert(target.end(), resolved_pins[pin_name].begin(), resolved_pins[pin_name].end());
                };
                add_pin_to("in", inputs);
                add_pin_to("out", outputs);
                graph.add_node(PrimitiveType::ROUTE, inputs, outputs, {}, type, part_path);
                continue; // Skip to the next part
            }

            // Fetch the blueprint for the part
            const Blueprint &part_bp = library.at(type);

            // Leaf nodes: Primitives, Peripherals and Macros
            if (part_bp.flags.contains("primitive") || part_bp.flags.contains("peripheral") || part_bp.flags.contains("macro")) {
                std::vector<NetID> inputs, outputs;
                std::vector<uint32_t> args;

                // Resolve generics (e.g. ADDR_SIZE and DATA_SIZE for RAM)
                for (const auto &g: generic_args) args.push_back(resolve_value(g, scope));

                // Helper to determine wire width based on generics
                auto get_pin_width = [&](const std::string &pin_name) -> uint32_t {
                    if (!part_bp.wires.contains(pin_name)) return 1;
                    uint32_t w = 1;
                    for (const auto &dim_str: part_bp.wires.at(pin_name).dims) {
                        auto it = std::ranges::find(part_bp.generic_params, dim_str);
                        if (it != part_bp.generic_params.end()) {
                            const size_t g_idx = std::distance(part_bp.generic_params.begin(), it);
                            if (g_idx < generic_args.size()) w *= resolve_value(generic_args[g_idx], scope);
                        } else {
                            try { w *= resolve_value(dim_str, scope); } catch (...) {}
                        }
                    }
                    return w;
                };

                // Helper to map resolved pins or pad unconnected ones with dummy nets
                auto add_pin_padded = [&](const std::string &pin_name, std::vector<NetID> &target) {
                    if (resolved_pins.contains(pin_name)) {
                        target.insert(target.end(), resolved_pins[pin_name].begin(), resolved_pins[pin_name].end());
                    } else {
                        const uint32_t w = get_pin_width(pin_name);
                        for(uint32_t b = 0; b < w; ++b) target.push_back(graph.add_net());
                    }
                };

                // Dynamically assign all inputs and outputs by reading the primitive's Blueprint!
                for (const std::string &in_wire: part_bp.in_wires) add_pin_padded(in_wire, inputs);
                for (const std::string &out_wire: part_bp.out_wires) add_pin_padded(out_wire, outputs);

                // Determine specific PrimitiveType enum and operator symbol
                auto p_type = PrimitiveType::DEAD;
                std::string op_symbol = part_path; // Default to path for debug

                if (part_bp.flags.contains("macro")) {
                    const std::string macro_val = *part_bp.flags.at("macro")[0];
                    if (macro_val == "Add") { p_type = PrimitiveType::MACRO_MATH; op_symbol = "+"; }
                    else if (macro_val == "Sub") { p_type = PrimitiveType::MACRO_MATH; op_symbol = "-"; }
                    else if (macro_val == "Mul") { p_type = PrimitiveType::MACRO_MATH; op_symbol = "*"; }
                    else if (macro_val == "Div") { p_type = PrimitiveType::MACRO_MATH; op_symbol = "/"; }
                    else if (macro_val == "And") { p_type = PrimitiveType::MACRO_MATH; op_symbol = "&"; }
                    else if (macro_val == "Or")  { p_type = PrimitiveType::MACRO_MATH; op_symbol = "|"; }
                    else if (macro_val == "Xor") { p_type = PrimitiveType::MACRO_MATH; op_symbol = "^"; }
                    else if (macro_val == "Shl") { p_type = PrimitiveType::MACRO_MATH; op_symbol = "<<"; }
                    else if (macro_val == "Shr") { p_type = PrimitiveType::MACRO_MATH; op_symbol = ">>"; }
                    else if (macro_val == "Eq")  { p_type = PrimitiveType::MACRO_COMPARE; op_symbol = "=="; }
                    else if (macro_val == "Neq") { p_type = PrimitiveType::MACRO_COMPARE; op_symbol = "!="; }
                    else if (macro_val == "Lt")  { p_type = PrimitiveType::MACRO_COMPARE; op_symbol = "<"; }
                    else if (macro_val == "Lte") { p_type = PrimitiveType::MACRO_COMPARE; op_symbol = "<="; }
                    else if (macro_val == "Gt")  { p_type = PrimitiveType::MACRO_COMPARE; op_symbol = ">"; }
                    else if (macro_val == "Gte") { p_type = PrimitiveType::MACRO_COMPARE; op_symbol = ">="; }
                    else if (macro_val == "Not") { p_type = PrimitiveType::MACRO_UNARY; op_symbol = "~"; }
                    else if (macro_val == "Mux") { p_type = PrimitiveType::MACRO_MUX; op_symbol = "Mux"; }
                    else if (macro_val == "DMux") { p_type = PrimitiveType::MACRO_DMUX; op_symbol = "DMux"; }
                    else throw std::runtime_error("Unknown @macro tag: " + macro_val);
                }
                else if (part_bp.flags.contains("primitive")) {
                    // Handles original Nand/Dff/RAM/ROM
                    if (type == "Nand") p_type = PrimitiveType::NAND;
                    else if (type == "Dff") p_type = PrimitiveType::DFF;
                    else if (type == "RAM") p_type = PrimitiveType::RAM;
                    else if (type == "ROM") p_type = PrimitiveType::ROM;
                    else if (type == "__Assign") p_type = PrimitiveType::ROUTE;
                    else throw std::runtime_error("Unknown primitive: " + type);
                }
                else if (part_bp.flags.contains("peripheral"))
                    p_type = PrimitiveType::PERIPHERAL;

                graph.add_node(p_type, inputs, outputs, args, type, op_symbol, active_tag);
            }
            // Standard hierarchical subchips
            else {
                Scope child_scope{part_path};
                for (size_t g = 0; g < generic_args.size(); ++g)
                    child_scope.values[part_bp.generic_params[g]] = resolve_value(generic_args[g], scope);

                expand_into_graph(type, child_scope, graph, depth + 1, active_tag);
                route_subchip_boundaries(part_bp, pins, scope, child_scope, graph, part_path);
            }
        }
    }

public:
    const std::unordered_set<std::string> internal_primitives{"__Assign"};
    std::unordered_set<std::string> injected_builtins;

    explicit Builder() { inject_builtins(); }

    void load_dependencies(const std::string &chip_name, const std::vector<std::string>& search_paths) {
        if (internal_primitives.contains(chip_name) || library.contains(chip_name)) return;
        if (std::ranges::find(loading_stack, chip_name) != loading_stack.end()) throw std::runtime_error("Circular dependency: " + chip_name);

        loading_stack.push_back(chip_name);

        // Parse file into a Blueprint
        Blueprint bp = hdl_to_blueprint(Utils::resolve_file(chip_name + ".hdl", search_paths), search_paths);

        // Resursively load subchips
        for (const Part &part: bp.parts) load_dependencies(part.type, search_paths);

        // Save blueprint to cache and pop stack
        library[chip_name] = std::move(bp);
        loading_stack.pop_back();
    }

    const Blueprint &get_blueprint(const std::string &name) { return library.at(name); }
    std::unordered_map<std::string, Blueprint> &get_library() { return library; }

    CompilerGraph build_local(const std::string &chip_name) const {
        CompilerGraph graph;
        Scope scope{chip_name};

        expand_into_graph(chip_name, scope, graph, 0);

        // Export only top-level IOs so Transpiler can generate API hooks
        const Blueprint& bp = library.at(chip_name);
        for (const std::string& io : bp.in_wires) {
            const NetID base = scope.nets.at(io);
            for(uint32_t i = 0; i < scope.widths.at(io); ++i) graph.io_mapping[io].push_back(base + i);
        }
        for (const std::string& io : bp.out_wires) {
            const NetID base = scope.nets.at(io);
            for(uint32_t i = 0; i < scope.widths.at(io); ++i) graph.io_mapping[io].push_back(base + i);
        }

        graph.resolve_aliases();
        return graph;
    }
};
