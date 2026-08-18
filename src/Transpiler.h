#pragma once
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <cctype>
#include "CompilerGraph.h"
#include "Builder.h"
#include "Utils.h"
#include "Optimiser.h"

class Transpiler {
public:
    /// Initiates the transpilation pipeline for the target chip.
    static void run_pipeline(const std::string& exe_dir, const std::string& target_chip, const std::vector<std::string>& search_paths, const std::string& output_dir) {
        Builder builder{};
        builder.load_dependencies(target_chip, search_paths);

        std::unordered_map<std::string, std::filesystem::file_time_type> latest_times;

        std::error_code ec;
        latest_times["__TRANSPILER_EXE__"] = std::filesystem::last_write_time(exe_dir, ec);

        compile_recursive(target_chip, builder, search_paths, output_dir, latest_times, true);
    }

private:
    /// Recursively evaluates dependencies, only rebuilding C++ files if their HDL source (or sub-chips) changed.
    static std::filesystem::file_time_type compile_recursive(
        const std::string &chip, Builder &builder,
        const std::vector<std::string> &search_paths,
        const std::string &output_dir,
        std::unordered_map<std::string, std::filesystem::file_time_type> &latest_times,
        const bool is_top_chip) {

        if (latest_times.contains(chip)) return latest_times[chip];

        const std::string hdl_path = Utils::resolve_file(chip + ".hdl", search_paths);
        const std::string cpp_path = std::format("{}{}.h", output_dir, chip);

        std::error_code ec;
        if (!std::filesystem::exists(hdl_path, ec)) throw std::runtime_error("Cannot locate HDL source file: " + hdl_path);

        auto max_time = std::filesystem::last_write_time(hdl_path, ec);
        max_time = std::max(max_time, latest_times["__TRANSPILER_EXE__"]); // Inject the transpiler's timestamp into the dependency evaluation

        // Inherit timestamps from constants
        const Blueprint &bp_check = builder.get_blueprint(chip);
        if (bp_check.flags.contains("const")) {
            for (const auto& const_val : bp_check.flags.at("const")) {
                if (const_val.has_value()) {
                    const std::string const_path = Utils::resolve_file(const_val.value() + ".const", search_paths);
                    const auto const_time = std::filesystem::last_write_time(const_path, ec);
                    if (!ec) max_time = std::max(max_time, const_time);
                }
            }
        }

        // Inherit timestamps from sub-chips
        for (const Blueprint &bp = builder.get_blueprint(chip); const Part &part: bp.parts) {
            if (!builder.internal_primitives.contains(part.type) && !builder.injected_builtins.contains(part.type))
                max_time = std::max(max_time, compile_recursive(part.type, builder, search_paths, output_dir, latest_times, false));
        }

        latest_times[chip] = max_time;

        // Recompile if necessary
        if (is_top_chip && Utils::is_outdated(max_time, cpp_path)) {
            CompilerGraph graph = builder.build_local(chip);

            if (!builder.get_blueprint(chip).flags.contains("peripheral"))
                Optimiser::optimise_graph(graph, builder.get_blueprint(chip));

            std::cout << "[Transpiler] Transpiling " << chip << "...";
            write_cpp(chip, cpp_path, builder, graph);
            std::cout << " COMPLETE\n";
        } else if (is_top_chip) {
            std::cout << "[Transpiler] " << chip << " is up to date. Skipping.\n";
        }

        return max_time;
    }

    /// Strips redundant matching parentheses from the boundaries of an expression.
    static std::string clean_expr(std::string expr) {
        if (expr.empty()) return expr;
        while (expr.length() >= 2 && expr.front() == '(' && expr.back() == ')') {
            int depth = 0;
            bool safe = true;
            for (size_t i = 0; i < expr.length() - 1; ++i) {
                if (expr[i] == '(') depth++;
                else if (expr[i] == ')') depth--;
                if (depth == 0) { safe = false; break; }
            }
            if (safe) expr = expr.substr(1, expr.length() - 2);
            else break;
        }
        return expr;
    }

    /// Wraps compound expressions safely in parentheses only if they rely on operator precedence.
    static std::string safe_wrap(const std::string& expr) {
        std::string cleaned = clean_expr(expr);
        int depth = 0;
        bool needs_wrap = false;
        for (const char c : cleaned) {
            if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') depth--;
            else if (depth == 0) {
                if (std::string("+-*/%&|^=?:<>").find(c) != std::string::npos) {
                    needs_wrap = true;
                    break;
                }
            }
        }
        return needs_wrap ? "(" + cleaned + ")" : cleaned;
    }

    /// Precomputes static binary string operations like "XULL << YULL" to reduce generated C++ overhead.
    static std::string fold_constants(std::string expr) {
        expr = clean_expr(expr);

        // Remove redundant zero operations
        if (expr.starts_with("0ULL | ")) expr = expr.substr(7);
        if (expr.ends_with(" | 0ULL")) expr = expr.substr(0, expr.length() - 7);
        if (expr.starts_with("0ULL ^ ")) expr = expr.substr(7);
        if (expr.ends_with(" ^ 0ULL")) expr = expr.substr(0, expr.length() - 7);
        if (expr.starts_with("0ULL & ") || expr.ends_with(" & 0ULL")) return "0ULL";
        if (expr.starts_with("0ULL << ")) return "0ULL";
        if (expr.starts_with("0ULL >> ")) return "0ULL";

        // Precompute raw literal math
        if (expr.find_first_not_of("0123456789ULL <<>>|&+-*^") == std::string::npos) {
            std::stringstream ss(expr);
            std::string op1, op, op2;
            ss >> op1 >> op >> op2;
            if (!op1.empty() && !op.empty() && !op2.empty() && op1.ends_with("ULL") && op2.ends_with("ULL")) {
                try {
                    const uint64_t v1 = std::stoull(op1);
                    const uint64_t v2 = std::stoull(op2);
                    if (op == "<<") return std::format("{}ULL", v1 << v2);
                    if (op == ">>") return std::format("{}ULL", v1 >> v2);
                    if (op == "|") return std::format("{}ULL", v1 | v2);
                    if (op == "&") return std::format("{}ULL", v1 & v2);
                    if (op == "^") return std::format("{}ULL", v1 ^ v2);
                    if (op == "+") return std::format("{}ULL", v1 + v2);
                    if (op == "-") return std::format("{}ULL", v1 - v2);
                    if (op == "*") return std::format("{}ULL", v1 * v2);
                } catch (...) {}
            }
        }
        return expr;
    }

    static void emit_class_header(std::ofstream& out, const std::string& chip_name, const CompilerGraph& graph) {
        out << "#ifndef " << chip_name << "_H\n";
        out << "#define " << chip_name << "_H\n\n";

        out << "#include <cstdint>\n";
        out << "#include <stdexcept>\n";
        out << "#include <string>\n";

        for (const auto& node : graph.nodes) {
            if (node.type == PrimitiveType::PERIPHERAL && node.chip_type == "Print") {
                out << "#include <iostream>\n";
                break;
            }
        }

        out << "\nclass " << chip_name << " final {\n";
        out << "    uint64_t state[" << graph.net_count << "] = {};\n\n";
    }

    static void emit_memory_blocks(std::ofstream& out, const CompilerGraph& graph) {
        out << "public:\n";
        bool found_memory = false;
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].type == PrimitiveType::RAM || graph.nodes[i].type == PrimitiveType::ROM) {
                found_memory = true;
                const uint32_t addr_w = graph.nodes[i].args[0];
                const uint32_t data_w = graph.nodes[i].args[1];
                if (data_w > Constants::MAX_BUS_WIDTH) throw std::runtime_error("Memory data width exceeds maximum bounds");
                std::string t_str = (data_w <= 8) ? "uint8_t" : (data_w <= 16) ? "uint16_t" : (data_w <= 32) ? "uint32_t" : "uint64_t";
                out << "    " << t_str << " mem_" << i << "[" << (1ULL << addr_w) << "] = {};\n";
            }
        }
        if (found_memory) out << "\n";
    }

    static void emit_peripherals(std::ofstream& out, const std::string& chip_name, const CompilerGraph& graph, Builder& builder, const std::unordered_map<NetID, uint64_t>& const_values) {
        for (size_t node_idx = 0; node_idx < graph.nodes.size(); ++node_idx) {
            const auto& node = graph.nodes[node_idx];
            if (node.type != PrimitiveType::PERIPHERAL) continue;

            const Blueprint& periph_bp = builder.get_blueprint(node.chip_type);
            std::string inst_name = node.chip_type;
            std::ranges::transform(inst_name, inst_name.begin(), tolower);
            inst_name += "_" + std::to_string(node_idx);

            out << "    struct " << node.chip_type << "_" << node_idx << "_t {\n";

            const bool has_io = !periph_bp.in_wires.empty() || !periph_bp.out_wires.empty();
            if (has_io) out << "        " << chip_name << "* chip{};\n\n";

            // Peripheral data binding
            if (node.chip_type == "Screen" && node.args.size() >= 2) {
                out << "        static constexpr uint32_t WIDTH = " << node.args[0] << ";\n";
                out << "        static constexpr uint32_t HEIGHT = " << node.args[1] << ";\n";
                out << "        uint32_t pixels[" << (node.args[0] * node.args[1]) << "] = {};\n\n";
            }

            // Getters
            uint32_t in_idx = 0;
            for (const auto& pin_name : periph_bp.in_wires) {
                if (in_idx < node.inputs.size()) {
                    NetID net = node.inputs[in_idx];
                    std::string get_str;

                    if (const_values.contains(net)) get_str = std::format("{}ULL", const_values.at(net));
                    else get_str = std::format("chip->state[{}]", net);

                    if (get_str.find("chip->state") == std::string::npos)
                        out << "        [[nodiscard]] static uint64_t get_" << pin_name << "() { return " << get_str << "; }\n";
                    else
                        out << "        [[nodiscard]] uint64_t get_" << pin_name << "() const { return " << get_str << "; }\n";
                    in_idx++;
                }
            }

            // Setters
            uint32_t out_idx = 0;
            for (const auto& pin_name : periph_bp.out_wires) {
                if (out_idx < node.outputs.size()) {
                    const NetID net = node.outputs[out_idx];
                    const uint32_t w = graph.net_widths[net];
                    std::string mask = w == 64 ? "~0ULL" : std::format("({}ULL)", (1ULL << w) - 1ULL);
                    out << "        void set_" << pin_name << "(const uint64_t val) const { chip->state[" << net << "] = val & " << mask << "; }\n";
                    out_idx++;
                }
            }

            // Native behaviors
            if (node.chip_type == "Screen") {
                out << "\n";
                out << "        void init() {\n";
                out << "            const uint32_t bg_col = get_bg();\n";
                out << "            for (uint32_t i = 0; i < WIDTH * HEIGHT; ++i) pixels[i] = bg_col;\n";
                out << "        }\n\n";

                out << "        void update() {\n";
                out << "            if (get_draw()) {\n";
                out << "                const uint64_t px = get_x(), py = get_y();\n";
                out << "                const uint64_t d = get_data(), s = get_span();\n";
                out << "                const uint32_t fg = get_fg(), bg = get_bg();\n";
                out << "                if (py < HEIGHT) {\n";
                out << "                    for (uint64_t i = 0; i < s; ++i) {\n";
                out << "                        if (px + i < WIDTH) pixels[py * WIDTH + px + i] = d >> i & 1ULL ? fg : bg;\n";
                out << "                    }\n";
                out << "                }\n";
                out << "            }\n";
                out << "        }\n";
            }
            else if (node.chip_type == "Audio") {
                out << "\n";
                out << "        static constexpr size_t BUFFER_SIZE = 4096;\n";
                out << "        uint64_t left_buf[BUFFER_SIZE] = {};\n";
                out << "        uint64_t right_buf[BUFFER_SIZE] = {};\n";
                out << "        size_t write_head = 0;\n";
                out << "        size_t read_head = 0;\n\n";

                out << "        void update() {\n";
                out << "            if (get_strobe()) {\n";
                out << "                left_buf[write_head] = get_left();\n";
                out << "                right_buf[write_head] = get_right();\n";
                out << "                write_head = (write_head + 1) % BUFFER_SIZE;\n";
                out << "            }\n";
                out << "        }\n";
            }
            else if (node.chip_type == "Print") {
                const uint32_t data_w = node.args.empty() ? 1 : node.args[0];
                out << "\n";
                out << "        void update() {\n";
                out << "            if (get_write()) {\n";
                out << "                const uint64_t mode = get_mode();\n";
                out << "                if (mode == 0) std::cout << static_cast<char>(get_data()) << std::flush;\n";
                out << "                else if (mode == 1) std::cout << get_data() << \"\\n\";\n";
                out << "                else if (mode == 2) std::cout << \"0x\" << std::hex << get_data() << std::dec << \"\\n\";\n";
                out << "                else if (mode == 3) {\n";
                out << "                    std::string b = \"0b\";\n";
                out << "                    const uint64_t v = get_data();\n";
                out << "                    for (int i = " << data_w << " - 1; i >= 0; --i) b += (v >> i) & 1ULL ? '1' : '0';\n";
                out << "                    std::cout << b << \"\\n\";\n";
                out << "                }\n";
                out << "            }\n";
                out << "        }\n";
            }

            if (has_io) out << "    } " << inst_name << "{this};\n\n";
            else out << "    } " << inst_name << "{};\n\n";
        }
    }

    static void emit_constructor(std::ofstream& out, const std::string& chip_name, const CompilerGraph& graph) {
        out << "    " << chip_name << "() {\n";
        out << "        state[" << NET_FALSE << "] = 0ULL;\n";
        out << "        state[" << NET_TRUE << "] = 1ULL;\n";
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].type == PrimitiveType::PERIPHERAL && graph.nodes[i].chip_type == "Screen")
                out << "        screen_" << i << ".init();\n";
        }
        out << "    }\n\n";
    }

    static void emit_io_api(std::ofstream& out, const CompilerGraph& graph) {
        std::unordered_map<std::string, NetID> all_pins = graph.io_mapping;
        std::unordered_map<std::string, size_t> mem_nodes;
        std::unordered_map<std::string, uint64_t> mem_caps;

        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].is_dead || graph.nodes[i].tag.empty()) continue;
            if (graph.nodes[i].type == PrimitiveType::RAM || graph.nodes[i].type == PrimitiveType::ROM) {
                mem_nodes[graph.nodes[i].tag] = i;
                mem_caps[graph.nodes[i].tag] = 1ULL << (!graph.nodes[i].args.empty() ? graph.nodes[i].args[0] : 1);
            }
            else if (graph.nodes[i].type == PrimitiveType::DFF) all_pins[graph.nodes[i].tag] = graph.nodes[i].outputs[0];
        }

        out << "    void set_pin(const std::string& name, const uint64_t value) {\n";
        for (const auto& [tag, idx] : mem_nodes) {
            out << "        if (name.starts_with(\"" << tag << "[\")) {\n";
            out << "            const size_t s = name.find('[') + 1, e = name.find(']');\n";
            out << "            if (s != std::string::npos && e != std::string::npos) { mem_" << idx << "[std::stoull(name.substr(s, e - s))] = value; return; }\n";
            out << "        }\n";
        }
        for (const auto& [name, net] : all_pins) {
            const uint32_t w = graph.net_widths[net];
            std::string mask = w == 64 ? "~0ULL" : std::format("{}ULL", (1ULL << w) - 1ULL);
            out << "        if (name == \"" << name << "\") { state[" << net << "] = value & " << mask << "; return; }\n";
        }
        out << "        if (name == \"clk\") return;\n";
        out << "        throw std::runtime_error(\"Pin not found for SET: \" + name);\n";
        out << "    }\n\n";

        out << "    [[nodiscard]] uint64_t get_pin(const std::string& name) const {\n";
        for (const auto& [tag, idx] : mem_nodes) {
            out << "        if (name.starts_with(\"" << tag << "[\")) {\n";
            out << "            const size_t s = name.find('[') + 1, e = name.find(']');\n";
            out << "            if (s != std::string::npos && e != std::string::npos) return mem_" << idx << "[std::stoull(name.substr(s, e - s))];\n";
            out << "        }\n";
        }
        for (const auto& [name, net] : all_pins)
            out << "        if (name == \"" << name << "\") return state[" << net << "];\n";
        out << "        throw std::runtime_error(\"Pin not found for GET: \" + name);\n";
        out << "    }\n\n";
    }

    static void emit_combinational(std::ofstream &out, const CompilerGraph &graph, const std::unordered_map<NetID, uint64_t>& const_values) {
        out << "    void update_combinational() {\n";

        // Calculate how many times each net is read to determine if it is single-use
        std::unordered_map<NetID, uint32_t> ref_counts;
        for (const auto& node : graph.nodes) {
            if (node.is_dead || node.type == PrimitiveType::DEAD || node.type == PrimitiveType::CONSTANT) continue;
            for (const NetID in : node.inputs) ref_counts[in]++;
        }

        std::unordered_map<NetID, std::string> inline_exprs;

        auto format_net = [&](const NetID net) {
            if (const_values.contains(net)) return std::format("{}ULL", const_values.at(net));
            if (inline_exprs.contains(net)) return safe_wrap(inline_exprs.at(net));
            if (net >= graph.net_count) return std::format("n_{}", net);
            return std::format("state[{}]", net);
        };

        auto format_associative = [&](const NetID net, const std::string& parent_op) {
            if (const_values.contains(net)) return std::format("{}ULL", const_values.at(net));
            if (inline_exprs.contains(net)) {
                std::string inner = inline_exprs.at(net);

                // Strip redundant brackets from associative trees to prevent cascading,
                // but enforce them strictly if mixing different operators to protect precedence.
                int depth = 0;
                bool needs_wrap = false;
                for (const char c : inner) {
                    if (c == '(' || c == '[' || c == '{') depth++;
                    else if (c == ')' || c == ']' || c == '}') depth--;
                    else if (depth == 0) {
                        if (std::string("+-*/%&|^=?:<>").find(c) != std::string::npos) {
                            if (parent_op.length() != 1 || c != parent_op[0]) {
                                needs_wrap = true;
                                break;
                            }
                        }
                    }
                }

                if (!needs_wrap) return inner;
                return safe_wrap(inner);
            }
            if (net >= graph.net_count) return std::format("n_{}", net);
            return std::format("state[{}]", net);
        };

        auto assign_net = [&](const NetID net, const std::string& expr, const bool requires_mask) {
            const uint32_t w = graph.net_widths[net];
            std::string final_expr = fold_constants(expr);

            // In RTL only mask if the expression can mathematically overflow past its bus width
            if (requires_mask && w < 64) {
                if (final_expr.find_first_not_of("0123456789ULL ") == std::string::npos) {
                    const uint64_t val = std::stoull(final_expr);
                    final_expr = std::format("{}ULL", val & ((1ULL << w) - 1ULL));
                } else {
                    final_expr = std::format("{} & {}ULL", safe_wrap(final_expr), (1ULL << w) - 1ULL);
                }
            } else {
                final_expr = clean_expr(final_expr);
            }

            if (net >= graph.net_count && ref_counts[net] == 1) {
                inline_exprs[net] = final_expr;
            } else if (net < graph.net_count || ref_counts[net] > 0) {
                if (net < graph.net_count) out << std::format("        state[{}] = {};\n", net, final_expr);
                else out << std::format("        const uint64_t n_{} = {};\n", net, final_expr);
            }
        };

        for (size_t i = 0; i < graph.nodes.size(); i++) {
            const CompNode& node = graph.nodes[i];
            if (node.is_dead || node.type == PrimitiveType::DEAD || node.type == PrimitiveType::CONSTANT) continue;

            switch (node.type) {
                case PrimitiveType::SLICE: {
                    const uint32_t shift_amount = std::min(node.args[0], node.args[1]);
                    if (shift_amount == 0) assign_net(node.outputs[0], format_net(node.inputs[0]), true);
                    else assign_net(node.outputs[0], fold_constants(std::format("{} >> {}ULL", format_net(node.inputs[0]), shift_amount)), true);
                    break;
                }
                case PrimitiveType::CONCAT: {
                    std::string expr = fold_constants(std::format("{} << 0ULL", format_net(node.inputs[0])));
                    uint32_t shift = graph.net_widths[node.inputs[0]];
                    for (size_t k = 1; k < node.inputs.size(); ++k) {
                        expr = fold_constants(std::format("{} | {}", expr, fold_constants(std::format("{} << {}ULL", format_net(node.inputs[k]), shift))));
                        shift += graph.net_widths[node.inputs[k]];
                    }
                    assign_net(node.outputs[0], expr, false);
                    break;
                }
                case PrimitiveType::ROUTE: {
                    for(size_t k = 0; k < node.outputs.size(); ++k) assign_net(node.outputs[k], format_net(node.inputs[k]), false);
                    break;
                }
                case PrimitiveType::AIG_AND: {
                    auto f_in = [&](const size_t idx, const uint32_t a) {
                        return a ? std::format("~{}", format_net(node.inputs[idx])) : format_net(node.inputs[idx]);
                    };
                    assign_net(node.outputs[0], std::format("{} & {}", f_in(0, node.args[0]), f_in(1, node.args[1])), false);
                    break;
                }
                case PrimitiveType::AIG_NOT: {
                    assign_net(node.outputs[0], std::format("~{}", format_net(node.inputs[0])), true);
                    break;
                }
                case PrimitiveType::RAM: {
                    assign_net(node.outputs[0], std::format("mem_{}[{}]", i, format_net(node.inputs[1])), false);
                    break;
                }
                case PrimitiveType::ROM: {
                    assign_net(node.outputs[0], std::format("mem_{}[{}]", i, format_net(node.inputs[0])), false);
                    break;
                }
                case PrimitiveType::MACRO_MATH: {
                    const bool can_overflow = (node.debug_name == "+" || node.debug_name == "-" || node.debug_name == "*" || node.debug_name == "<<");
                    std::string a_expr, b_expr;

                    if (node.debug_name == "|" || node.debug_name == "&" || node.debug_name == "^") {
                        a_expr = format_associative(node.inputs[0], node.debug_name);
                        b_expr = format_associative(node.inputs[1], node.debug_name);
                    } else {
                        a_expr = format_net(node.inputs[0]);
                        b_expr = format_net(node.inputs[1]);
                    }

                    assign_net(node.outputs[0], std::format("{} {} {}", a_expr, node.debug_name, b_expr), can_overflow);
                    break;
                }
                case PrimitiveType::MACRO_COMPARE: {
                    assign_net(node.outputs[0], std::format("{} {} {}", format_net(node.inputs[0]), node.debug_name, format_net(node.inputs[1])), false);
                    break;
                }
                case PrimitiveType::MACRO_UNARY: {
                    assign_net(node.outputs[0], std::format("{}{}", node.debug_name, format_net(node.inputs[0])), true);
                    break;
                }
                case PrimitiveType::MACRO_MUX: {
                    std::string sel = format_net(node.inputs[2]);
                    if (sel == "0ULL") assign_net(node.outputs[0], format_net(node.inputs[0]), false);
                    else if (sel == "1ULL") assign_net(node.outputs[0], format_net(node.inputs[1]), false);
                    else if (format_net(node.inputs[0]) == format_net(node.inputs[1])) assign_net(node.outputs[0], format_net(node.inputs[0]), false);
                    else assign_net(node.outputs[0], std::format("{} ? {} : {}", sel, format_net(node.inputs[1]), format_net(node.inputs[0])), false);
                    break;
                }
                case PrimitiveType::MACRO_DMUX: {
                    std::string sel = format_net(node.inputs[1]);
                    std::string in_val = format_net(node.inputs[0]);

                    if (in_val == "0ULL") {
                        assign_net(node.outputs[0], "0ULL", false);
                        assign_net(node.outputs[1], "0ULL", false);
                    } else if (sel == "0ULL") {
                        assign_net(node.outputs[0], in_val, false);
                        assign_net(node.outputs[1], "0ULL", false);
                    } else if (sel == "1ULL") {
                        assign_net(node.outputs[0], "0ULL", false);
                        assign_net(node.outputs[1], in_val, false);
                    } else {
                        assign_net(node.outputs[0], std::format("{} ? 0ULL : {}", sel, in_val), false);
                        assign_net(node.outputs[1], std::format("{} ? {} : 0ULL", sel, in_val), false);
                    }
                    break;
                }
                case PrimitiveType::NAND: {
                    assign_net(node.outputs[0], std::format("~({} & {})", format_net(node.inputs[0]), format_net(node.inputs[1])), true);
                    break;
                }
                default: break;
            }
        }
        out << "    }\n\n";
    }

    static void emit_sequential(std::ofstream& out, const CompilerGraph& graph) {
        out << "    void update_sequential() {\n";

        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].is_dead || graph.nodes[i].type != PrimitiveType::RAM) continue;
            out << std::format("        if (state[{}]) mem_{}[state[{}]] = state[{}];\n", graph.nodes[i].inputs[2], i, graph.nodes[i].inputs[1], graph.nodes[i].inputs[0]);
        }

        std::vector<std::pair<NetID, NetID>> dffs;
        for (const auto& node : graph.nodes) {
            if (node.type == PrimitiveType::DFF && !node.is_dead) {
                if (node.inputs[0] != node.outputs[0]) dffs.emplace_back(node.inputs[0], node.outputs[0]);
            }
        }

        // Buffer reads to prevent sequential shift-register corruption
        for (size_t i = 0; i < dffs.size(); ++i) out << std::format("        const uint64_t dff_{} = state[{}];\n", i, dffs[i].first);
        for (size_t i = 0; i < dffs.size(); ++i) out << std::format("        state[{}] = dff_{};\n", dffs[i].second, i);

        out << "    }\n};\n";
    }

    static void emit_dynamic_wrapper(std::ofstream& out, const std::string& chip_name, const CompilerGraph& graph, const Blueprint& bp) {
        int screen_idx = -1;
        std::vector<size_t> keyboards;
        std::vector<size_t> generic_peripherals;

        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].type == PrimitiveType::PERIPHERAL) {
                if (graph.nodes[i].chip_type == "Screen") {
                    screen_idx = static_cast<int>(i);
                    generic_peripherals.push_back(i); // Screen still needs update() called
                }
                else if (graph.nodes[i].chip_type == "Keyboard")
                    keyboards.push_back(i);
                else
                    generic_peripherals.push_back(i);
            }
        }

        double target_hz = 0.0;
        if (bp.flags.contains("toplevel") && !bp.flags.at("toplevel").empty() && bp.flags.at("toplevel")[0].has_value())
            target_hz = std::stod(bp.flags.at("toplevel")[0].value());

        out << "\n#ifdef BUILD_DYNAMIC_LIBRARY\n";
        out << "#include \"IDynamicChip.h\"\n\n";

        out << "class DynamicWrapper final : public IDynamicChip {\n";
        out << "    " << chip_name << " chip;\n";
        out << "public:\n";
        out << "    void set_pin(const char* name, uint64_t value) override { if (name) chip.set_pin(name, value); }\n";
        out << "    uint64_t get_pin(const char* name) override { return name ? chip.get_pin(name) : 0; }\n\n";

        out << "    HardwareState get_info() override {\n";
        out << "        HardwareState state{};\n";
        out << "        state.target_hz = " << target_hz << ";\n";
        if (screen_idx != -1) {
            out << "        state.has_screen = true;\n";
            out << "        state.screen_width = chip.screen_" << screen_idx << ".WIDTH;\n";
            out << "        state.screen_height = chip.screen_" << screen_idx << ".HEIGHT;\n";
            out << "        state.screen_pixels = chip.screen_" << screen_idx << ".pixels;\n";
        }
        out << "        return state;\n";
        out << "    }\n\n";

        out << "    void emulate_frame(const uint64_t cycles, const uint64_t k_low, const uint64_t k_high) override {\n";

        // Implant keyboard updates
        for (const size_t k : keyboards)
            out << "        chip.keyboard_" << k << ".set_low_keys(k_low); chip.keyboard_" << k << ".set_high_keys(k_high);\n";

        out << "        for (uint64_t i = 0; i < cycles; ++i) {\n";
        out << "            chip.update_combinational();\n";

        // Implant peripheral updates into the loop
        for (const size_t p : generic_peripherals) {
            std::string inst = graph.nodes[p].chip_type;
            std::ranges::transform(inst, inst.begin(), ::tolower);
            out << "            chip." << inst << "_" << p << ".update();\n";
        }

        out << "            chip.update_sequential();\n";
        out << "        }\n";
        out << "        chip.update_combinational();\n";
        out << "    }\n};\n\n";

        out << "extern \"C\" __declspec(dllexport) IDynamicChip* create_chip() { return new DynamicWrapper(); }\n";
        out << "extern \"C\" __declspec(dllexport) void destroy_chip(const IDynamicChip* ptr) { delete ptr; }\n";
        out << "#endif // BUILD_DYNAMIC_LIBRARY\n\n";
    }

    static void write_cpp(const std::string &chip_name, const std::string &output_path, Builder &builder, const CompilerGraph &graph) {
        const Blueprint &bp = builder.get_blueprint(chip_name);
        std::ofstream out_file(output_path);

        // Precompute all constants globally
        std::unordered_map<NetID, uint64_t> const_values;
        const_values[NET_FALSE] = 0ULL;
        const_values[NET_TRUE] = 1ULL;
        for (const auto& node : graph.nodes) {
            if (!node.is_dead && node.type == PrimitiveType::CONSTANT)
                const_values[node.outputs[0]] = static_cast<uint64_t>(node.args[0]) | (static_cast<uint64_t>(node.args[1]) << 32ULL);
        }

        emit_class_header(out_file, chip_name, graph);
        emit_memory_blocks(out_file, graph);
        emit_peripherals(out_file, chip_name, graph, builder, const_values);
        emit_constructor(out_file, chip_name, graph);
        emit_io_api(out_file, graph);
        emit_combinational(out_file, graph, const_values);
        emit_sequential(out_file, graph);
        emit_dynamic_wrapper(out_file, chip_name, graph, bp);

        out_file << "#endif // " << chip_name << "_H\n";
    }
};