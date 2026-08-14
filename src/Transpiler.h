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

    static void replace_all(std::string& str, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    }

    /// Strips redundant parentheses from an expression string to keep C++ output clean.
    static std::string clean_expr(std::string expr) {
        if (expr.empty()) return expr;
        while (expr.length() >= 2 && expr.front() == '(' && expr.back() == ')') {
            int depth = 0;
            bool safe = true;
            for (size_t i = 0; i < expr.length() - 1; ++i) {
                if (expr[i] == '(') depth++;
                else if (expr[i] == ')') depth--;
                if (depth == 0 && i != 0) { safe = false; break; }
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
                if (std::string("+-*/%&|^!=?:~").find(c) != std::string::npos) {
                    needs_wrap = true;
                    break;
                }
                if (c == '<' || c == '>') {
                    // Ignore template brackets inside atomic bus accesses
                    if (!cleaned.starts_with("read_bus") && !cleaned.starts_with("write_bus")) {
                        needs_wrap = true;
                        break;
                    }
                }
            }
        }
        return needs_wrap ? "(" + cleaned + ")" : cleaned;
    }

    /// Converts a hardware NetID bus (like [12, 13, 14]) into an optimized C++ read expression (e.g. read_bus<3, 1>(12)).
    static std::pair<std::string, uint32_t> emit_bus_read(const std::vector<NetID>& nets, uint32_t start_idx, const uint32_t width) {
        if (width == 0) return {"0ULL", start_idx};

        uint64_t constant_part = 0;
        std::vector<std::string> parts;

        for (uint32_t i = 0; i < width; ) {
            const NetID net = nets[start_idx + i];

            // Fold hardware constants directly into the expression mask
            if (net == 0) { i++; continue; }
            if (net == 1) { constant_part |= (1ULL << i); i++; continue; }

            // Lookahead for contiguous vectors
            uint32_t stride_len = 1;
            int64_t step = 0;
            if (i + 1 < width && nets[start_idx + i + 1] > 1) {
                step = static_cast<int64_t>(nets[start_idx + i + 1]) - static_cast<int64_t>(net);
                stride_len = 2;
                while (i + stride_len < width) {
                    const NetID next_net = nets[start_idx + i + stride_len];
                    if (next_net <= 1 || static_cast<int64_t>(next_net) - static_cast<int64_t>(nets[start_idx + i + stride_len - 1]) != step) break;
                    stride_len++;
                }
            }

            // If 3+ nets follow a rigid mathematical step, compress them into a `read_bus` chunk
            if (stride_len >= 3) {
                std::string call = std::format("read_bus<{}, {}, {}>()", stride_len, step, net);
                if (i > 0) call = std::format("{} << {}ULL", call, i);
                parts.push_back(call);
                i += stride_len;
            } else {
                if (i == 0) parts.push_back(std::format("(state[{0} / 64] >> ({0} % 64)) & 1ULL", net));
                else parts.push_back(std::format("((state[{0} / 64] >> ({0} % 64)) & 1ULL) << {1}ULL", net, i));
                i++;
            }
        }

        std::stringstream expr;
        expr << "(";
        bool first = true;
        if (constant_part != 0 || parts.empty()) {
            expr << constant_part << "ULL";
            first = false;
        }
        for (const auto& p : parts) {
            if (!first) expr << " | ";
            expr << p;
            first = false;
        }
        expr << ")";
        return {expr.str(), start_idx + width};
    }

    /// Evaluates how many C++ assignments a bus write will require, determining if temporary variables are needed.
    static uint32_t count_write_statements(const std::vector<NetID>& nets, const uint32_t start_idx, const uint32_t width) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < width; ) {
            const NetID net = nets[start_idx + i];
            if (net <= 1) { i++; continue; }
            uint32_t stride_len = 1;
            if (i + 1 < width && nets[start_idx + i + 1] > 1) {
                const int64_t step = static_cast<int64_t>(nets[start_idx + i + 1]) - static_cast<int64_t>(net);
                stride_len = 2;
                while (i + stride_len < width) {
                    const NetID next_net = nets[start_idx + i + stride_len];
                    if (next_net <= 1 || static_cast<int64_t>(next_net) - static_cast<int64_t>(nets[start_idx + i + stride_len - 1]) != step) break;
                    stride_len++;
                }
            }
            count++;
            i += (stride_len >= 3) ? stride_len : 1;
        }
        return count;
    }

    /// Converts a hardware NetID bus into optimized C++ write expressions, emitting `write_bus` for packed vectors.
    static std::pair<std::string, uint32_t> emit_bus_write(const std::vector<NetID>& nets, uint32_t start_idx, const uint32_t width, const std::string& expr, uint32_t node_idx = 0) {
        if (width == 0) return {"", start_idx};
        const std::string clean_e = clean_expr(expr);
        std::string val_name = safe_wrap(clean_e);
        std::stringstream os;

        // If the expression is complex and needs to be fragmented across multiple writes, cache it in a register
        const uint32_t stmt_count = count_write_statements(nets, start_idx, width);
        if (stmt_count > 1 && clean_e.find(' ') != std::string::npos) {
            os << std::format("const uint64_t temp_{}_{} = {}; ", node_idx, start_idx, clean_e);
            val_name = std::format("temp_{}_{}", node_idx, start_idx);
        }

        for (uint32_t i = 0; i < width; ) {
            const NetID net = nets[start_idx + i];
            if (net <= 1) { i++; continue; } // Hardware Constants 0 and 1 are Read-Only

            uint32_t stride_len = 1;
            int64_t step = 0;
            if (i + 1 < width && nets[start_idx + i + 1] > 1) {
                step = static_cast<int64_t>(nets[start_idx + i + 1]) - static_cast<int64_t>(net);
                stride_len = 2;
                while (i + stride_len < width) {
                    const NetID next_net = nets[start_idx + i + stride_len];
                    if (next_net <= 1 || static_cast<int64_t>(next_net) - static_cast<int64_t>(nets[start_idx + i + stride_len - 1]) != step) break;
                    stride_len++;
                }
            }

            if (stride_len >= 3) {
                if (i == 0) os << std::format("write_bus<{}, {}, {}>({}); ", stride_len, step, net, val_name);
                else os << std::format("write_bus<{}, {}, {}>({} >> {}ULL); ", stride_len, step, net, val_name, i);
                i += stride_len;
            } else {
                if (i == 0) os << std::format("state[{0} / 64] = (state[{0} / 64] & ~(1ULL << ({0} % 64))) | (({1} & 1ULL) << ({0} % 64)); ", net, val_name);
                else os << std::format("state[{0} / 64] = (state[{0} / 64] & ~(1ULL << ({0} % 64))) | ((({1} >> {2}ULL) & 1ULL) << ({0} % 64)); ", net, val_name, i);
                i++;
            }
        }
        return {os.str(), start_idx + width};
    }

    static void emit_class_header(std::ofstream& out, const std::string& chip_name, const CompilerGraph& graph) {
        out << "#ifndef " << chip_name << "_H\n";
        out << "#define " << chip_name << "_H\n\n";

        out << "#include <cstdint>\n";
        out << "#include <stdexcept>\n";
        out << "#include <string>\n";
        out << "#include <iostream>\n";
        out << "#include <fstream>\n";
        out << "#include <vector>\n\n";

        out << "class " << chip_name << " final {\n";
        out << "    uint64_t state[" << (graph.net_count + 63) / 64 << "] = {};\n\n";
    }

    static void emit_bus_helpers(std::ofstream& out) {
        out << "    template <uint64_t Width, int64_t Step, uint64_t Base>\n";
        out << "    [[nodiscard]] inline uint64_t read_bus() const {\n";
        out << "        constexpr uint64_t W_MASK = (Width == 64) ? ~0ULL : ((1ULL << Width) - 1ULL);\n";
        out << "        if constexpr (Step == 1 && (Base % 64) + Width <= 64) {\n";
        out << "            return (state[Base / 64] >> (Base % 64)) & W_MASK;\n";
        out << "        } else if constexpr (Step == 1 && (Base % 64) + Width > 64) {\n";
        out << "            const uint64_t split = 64 - (Base % 64);\n";
        out << "            const uint64_t low = state[Base / 64] >> (Base % 64);\n";
        out << "            const uint64_t high = state[(Base / 64) + 1] & (((1ULL << (Width - split)) - 1ULL));\n";
        out << "            return low | (high << split);\n";
        out << "        } else {\n";
        out << "            uint64_t res = 0;\n";
        out << "            #pragma GCC unroll 64\n";
        out << "            for (uint64_t i = 0; i < Width; ++i) {\n";
        out << "                const uint64_t idx = Base + i * Step;\n";
        out << "                res |= (((state[idx / 64] >> (idx % 64)) & 1ULL) << i);\n";
        out << "            }\n";
        out << "            return res;\n";
        out << "        }\n";
        out << "    }\n\n";

        out << "    template <uint64_t Width, int64_t Step, uint64_t Base>\n";
        out << "    inline void write_bus(const uint64_t val) {\n";
        out << "        constexpr uint64_t W_MASK = (Width == 64) ? ~0ULL : ((1ULL << Width) - 1ULL);\n";
        out << "        if constexpr (Step == 1 && (Base % 64) + Width <= 64) {\n";
        out << "            const uint64_t mask = W_MASK << (Base % 64);\n";
        out << "            state[Base / 64] = (state[Base / 64] & ~mask) | ((val << (Base % 64)) & mask);\n";
        out << "        } else if constexpr (Step == 1 && (Base % 64) + Width > 64) {\n";
        out << "            const uint64_t split = 64 - (Base % 64);\n";
        out << "            const uint64_t mask_low = ((1ULL << split) - 1ULL) << (Base % 64);\n";
        out << "            state[Base / 64] = (state[Base / 64] & ~mask_low) | ((val << (Base % 64)) & mask_low);\n";
        out << "            const uint64_t mask_high = (1ULL << (Width - split)) - 1ULL;\n";
        out << "            state[(Base / 64) + 1] = (state[(Base / 64) + 1] & ~mask_high) | ((val >> split) & mask_high);\n";
        out << "        } else {\n";
        out << "            #pragma GCC unroll 64\n";
        out << "            for (uint64_t i = 0; i < Width; ++i) {\n";
        out << "                const uint64_t idx = Base + i * Step;\n";
        out << "                state[idx / 64] = (state[idx / 64] & ~(1ULL << (idx % 64))) | (((val >> i) & 1ULL) << (idx % 64));\n";
        out << "            }\n";
        out << "        }\n";
        out << "    }\n\n";
    }

    static void emit_memory_blocks(std::ofstream& out, const CompilerGraph& graph) {
        out << "public:\n";
        bool found_memory = false;
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].type == PrimitiveType::RAM || graph.nodes[i].type == PrimitiveType::ROM) {
                found_memory = true;
                const uint32_t addr_w = graph.nodes[i].args[0];
                const uint32_t data_w = graph.nodes[i].args[1];
                std::string t_str = (data_w <= 8) ? "uint8_t" : (data_w <= 16) ? "uint16_t" : "uint32_t";
                out << "    " << t_str << " mem_" << i << "[" << (1ULL << addr_w) << "] = {};\n";
            }
        }
        if (found_memory) out << "\n";
    }

    static void emit_peripherals(std::ofstream& out, const std::string& chip_name, const CompilerGraph& graph, Builder& builder) {
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

            auto resolve_width = [&](const std::string& p_name) -> uint32_t {
                if (!periph_bp.wires.contains(p_name)) return 1;
                uint32_t w = 1;
                for (const auto& d : periph_bp.wires.at(p_name).dims) {
                    auto it = std::ranges::find(periph_bp.generic_params, d);
                    if (it != periph_bp.generic_params.end()) {
                        if (const size_t idx = std::distance(periph_bp.generic_params.begin(), it); idx < node.args.size()) w *= node.args[idx];
                    } else { try { w *= std::stoi(d); } catch(...) {} }
                }
                return w;
            };

            // Getters
            uint32_t in_idx = 0;
            for (const auto& pin_name : periph_bp.in_wires) {
                const uint32_t pin_w = resolve_width(pin_name);
                if (in_idx < node.inputs.size()) {
                    auto [expr, next_idx] = emit_bus_read(node.inputs, in_idx, pin_w);
                    std::string get_str = clean_expr(expr);

                    const bool is_static = get_str.find("state[") == std::string::npos && get_str.find("read_bus<") == std::string::npos;

                    replace_all(get_str, "state[", "chip->state[");
                    replace_all(get_str, "read_bus<", "chip->read_bus<");

                    if (is_static)
                        out << "        [[nodiscard]] static uint64_t get_" << pin_name << "() { return " << get_str << "; }\n";
                    else
                        out << "        [[nodiscard]] uint64_t get_" << pin_name << "() const { return " << get_str << "; }\n";

                    in_idx = next_idx;
                }
            }

            // Setters
            uint32_t out_idx = 0;
            for (const auto& pin_name : periph_bp.out_wires) {
                const uint32_t pin_w = resolve_width(pin_name);
                if (out_idx < node.outputs.size()) {
                    auto [set_str, next_idx] = emit_bus_write(node.outputs, out_idx, pin_w, "val", node_idx);
                    out_idx = next_idx;

                    replace_all(set_str, "state[", "chip->state[");
                    replace_all(set_str, "write_bus<", "chip->write_bus<");

                    out << "        void set_" << pin_name << "(const uint64_t val) const { " << set_str << "}\n";
                }
            }

            // Native behaviors
            if (node.chip_type == "Screen") {
                out << "\n";
                out << "        void init() {\n";
                out << "            const uint32_t bg_col = get_bg();\n";
                out << "            for (uint32_t i = 0; i < WIDTH * HEIGHT; ++i) pixels[i] = bg_col;\n";
                out << "        }\n\n";

                out << "        inline void update() {\n";
                out << "            if (get_draw()) {\n";
                out << "                const uint64_t px = get_x(), py = get_y();\n";
                out << "                const uint64_t d = get_data(), s = get_span();\n";
                out << "                const uint32_t fg = get_fg(), bg = get_bg();\n";
                out << "                if (py < HEIGHT) {\n";
                out << "                    for (uint64_t i = 0; i < s; ++i) {\n";
                out << "                        if (px + i < WIDTH) pixels[py * WIDTH + px + i] = (d >> i) & 1ULL ? fg : bg;\n";
                out << "                    }\n";
                out << "                }\n";
                out << "            }\n";
                out << "        }\n";
            } else if (node.chip_type == "Print") {
                const uint32_t data_w = node.args.empty() ? 1 : node.args[0];
                out << "\n";
                out << "        inline void update() {\n";
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
        out << "        state[" << (NET_FALSE / 64) << "] &= ~(1ULL << (" << (NET_FALSE % 64) << "));\n";
        out << "        state[" << (NET_TRUE / 64) << "] |= (1ULL << (" << (NET_TRUE % 64) << "));\n";
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].type == PrimitiveType::PERIPHERAL && graph.nodes[i].chip_type == "Screen")
                out << "        screen_" << i << ".init();\n";
        }
        out << "    }\n\n";
    }

    static void emit_rom_loader(std::ofstream& out, const CompilerGraph& graph) {
        out << "    void load_rom(const std::string& tag, const std::string& rom_path) {\n";
        out << "        if (rom_path.empty() || tag.empty()) return;\n\n";
        out << "        std::ifstream file(rom_path);\n";
        out << "        if (!file.is_open()) {\n";
        out << "            std::cerr << \"[ROM] Warning: Could not open ROM file: \" << rom_path << \"\\n\";\n";
        out << "            return;\n";
        out << "        }\n\n";
        out << "        std::vector<uint16_t> buffer;\n";
        out << "        std::string line;\n";
        out << "        while (std::getline(file, line)) {\n";
        out << "            if (const size_t pos = line.find(\"//\"); pos != std::string::npos) line = line.substr(0, pos);\n";
        out << "            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();\n";
        out << "            size_t start = 0;\n";
        out << "            while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) start++;\n";
        out << "            if (start >= line.size()) continue;\n";
        out << "            line = line.substr(start);\n";
        out << "            if (line.empty() || (line[0] != '0' && line[0] != '1')) continue;\n\n";
        out << "            uint16_t val = 0;\n";
        out << "            for (size_t i = 0; i < 16 && i < line.length(); ++i) {\n";
        out << "                if (line[i] == '1') val |= (1ULL << (15 - i));\n";
        out << "            }\n";
        out << "            buffer.push_back(val);\n";
        out << "        }\n\n";

        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].type == PrimitiveType::ROM && !graph.nodes[i].tag.empty()) {
                const uint64_t cap = 1ULL << (!graph.nodes[i].args.empty() ? graph.nodes[i].args[0] : 1);
                out << "        if (tag == \"" << graph.nodes[i].tag << "\") {\n";
                out << "            const size_t loaded = std::min(buffer.size(), " << cap << "ULL);\n";
                out << "            for (size_t j = 0; j < loaded; ++j) mem_" << i << "[j] = buffer[j];\n";
                out << R"(            std::cout << "[ROM] Loaded " << loaded << "/" << )" << cap << "ULL << \" words into '\" << tag << \"'\\n\";\n";
                out << "        }\n";
            }
        }
        out << "    }\n\n";
    }

    static void emit_io_api(std::ofstream& out, const CompilerGraph& graph) {
        std::unordered_map<std::string, std::vector<NetID>> all_pins = graph.io_mapping;
        std::unordered_map<std::string, size_t> mem_nodes;

        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].is_dead || graph.nodes[i].tag.empty()) continue;
            if (graph.nodes[i].type == PrimitiveType::RAM || graph.nodes[i].type == PrimitiveType::ROM) mem_nodes[graph.nodes[i].tag] = i;
            else if (graph.nodes[i].type == PrimitiveType::DFF) {
                for (NetID net : graph.nodes[i].outputs) all_pins[graph.nodes[i].tag].push_back(net);
            }
        }

        out << "    void set_pin(const std::string& name, const uint64_t value) {\n";
        for (const auto& [tag, idx] : mem_nodes) {
            out << "        if (name.starts_with(\"" << tag << "[\")) {\n";
            out << "            const size_t s = name.find('[') + 1, e = name.find(']');\n";
            out << "            if (s != std::string::npos && e != std::string::npos) { mem_" << idx << "[std::stoull(name.substr(s, e - s))] = value; return; }\n";
            out << "        }\n";
        }
        for (const auto& [name, nets] : all_pins) {
            auto [write_str, _] = emit_bus_write(nets, 0, nets.size(), "value", 99999);
            out << "        if (name == \"" << name << "\") { " << write_str << "return; }\n";
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
        for (const auto& [name, nets] : all_pins)
            out << "        if (name == \"" << name << "\") return " << clean_expr(emit_bus_read(nets, 0, nets.size()).first) << ";\n";
        out << "        throw std::runtime_error(\"Pin not found for GET: \" + name);\n";
        out << "    }\n\n";
    }

    static void emit_combinational(std::ofstream &out, const CompilerGraph &graph) {
        const Vectorizer vectorizer(graph);

        out << "    void update_combinational() {\n";

        // Calculate how many times each net is read to determine if it is single-use
        std::unordered_map<NetID, uint32_t> ref_counts;
        for (const auto& node : graph.nodes) {
            if (node.is_dead || node.type == PrimitiveType::DEAD) continue;
            for (const NetID in : node.inputs) ref_counts[in]++;
        }

        std::unordered_map<NetID, std::string> inline_exprs;

        // Formats nets, transparently wrapping and inserting single-use expressions where applicable
        auto format_net = [&](const NetID net) {
            if (inline_exprs.contains(net)) return safe_wrap(inline_exprs[net]);
            return net < graph.net_count ? safe_wrap(std::format("(state[{0} / 64] >> ({0} % 64)) & 1ULL", net)) : std::format("n_{}", net);
        };
        auto format_in = [&](const CompNode& node, const size_t idx, const uint32_t arg) {
            return arg ? safe_wrap(std::format("{} ^ 1", format_net(node.inputs[idx]))) : format_net(node.inputs[idx]);
        };

        for (size_t i = 0; i < graph.nodes.size(); i++) {
            const CompNode& node = graph.nodes[i];
            if (node.is_dead || node.type == PrimitiveType::DEAD) continue;

            if (vectorizer.handle_node(out, i, node)) continue;

            switch (node.type) {
                case PrimitiveType::ROUTE: {
                    for(size_t k = 0; k < node.outputs.size(); ++k) {
                        const NetID out_net = node.outputs[k];
                        std::string expr = format_net(node.inputs[k]);

                        if (out_net >= graph.net_count && ref_counts[out_net] == 1) {
                            inline_exprs[out_net] = clean_expr(expr);
                        } else if (out_net < graph.net_count || ref_counts[out_net] > 0) {
                            if (out_net < graph.net_count) out << std::format("        state[{0} / 64] = (state[{0} / 64] & ~(1ULL << ({0} % 64))) | (static_cast<uint64_t>({1}) << ({0} % 64));\n", out_net, expr);
                            else out << std::format("        const uint8_t n_{} = {};\n", out_net, expr);
                        }
                    }
                    break;
                }
                case PrimitiveType::AIG_AND: {
                    const NetID out_net = node.outputs[0];
                    std::string expr = std::format("{} & {}", format_in(node, 0, node.args[0]), format_in(node, 1, node.args[1]));

                    if (out_net >= graph.net_count && ref_counts[out_net] == 1) {
                        inline_exprs[out_net] = clean_expr(expr);
                    } else if (out_net < graph.net_count || ref_counts[out_net] > 0) {
                        if (out_net < graph.net_count) out << std::format("        state[{0} / 64] = (state[{0} / 64] & ~(1ULL << ({0} % 64))) | (static_cast<uint64_t>({1}) << ({0} % 64));\n", out_net, expr);
                        else out << std::format("        const uint8_t n_{} = {};\n", out_net, expr);
                    }
                    break;
                }
                case PrimitiveType::AIG_NOT: {
                    const NetID out_net = node.outputs[0];
                    std::string expr = std::format("{} ^ 1", format_net(node.inputs[0]));

                    if (out_net >= graph.net_count && ref_counts[out_net] == 1) {
                        inline_exprs[out_net] = clean_expr(expr);
                    } else if (out_net < graph.net_count || ref_counts[out_net] > 0) {
                        if (out_net < graph.net_count) out << std::format("        state[{0} / 64] = (state[{0} / 64] & ~(1ULL << ({0} % 64))) | (static_cast<uint64_t>({1}) << ({0} % 64));\n", out_net, expr);
                        else out << std::format("        const uint8_t n_{} = {};\n", out_net, expr);
                    }
                    break;
                }
                case PrimitiveType::RAM:
                case PrimitiveType::ROM: {
                    const uint32_t addr_w = node.args[0];
                    const uint32_t data_w = node.args[1];
                    auto [a_expr, _1] = emit_bus_read(node.inputs, (node.type == PrimitiveType::RAM) ? data_w : 0, addr_w);
                    auto [write_str, _2] = emit_bus_write(node.outputs, 0, data_w, std::format("mem_{}[{}]", i, a_expr), i);
                    out << "        " << write_str << "\n";
                    break;
                }
                case PrimitiveType::MACRO_MATH: {
                    const uint32_t w = node.outputs.size();
                    auto [a_expr, next] = emit_bus_read(node.inputs, 0, w);
                    auto [b_expr, _] = emit_bus_read(node.inputs, next, w);
                    auto [write_str, _2] = emit_bus_write(node.outputs, 0, w, std::format("{} {} {}", a_expr, node.debug_name, b_expr), i);
                    out << "        " << write_str << "\n";
                    break;
                }
                case PrimitiveType::MACRO_COMPARE: {
                    const uint32_t w = node.inputs.size() / 2;
                    auto [a_expr, next] = emit_bus_read(node.inputs, 0, w);
                    auto [b_expr, _] = emit_bus_read(node.inputs, next, w);
                    auto [write_str, _2] = emit_bus_write(node.outputs, 0, 1, std::format("{} {} {}", a_expr, node.debug_name, b_expr), i);
                    out << "        " << write_str << "\n";
                    break;
                }
                case PrimitiveType::MACRO_UNARY: {
                    const uint32_t w = node.outputs.size();
                    auto [in_expr, _] = emit_bus_read(node.inputs, 0, w);
                    auto [write_str, _2] = emit_bus_write(node.outputs, 0, w, std::format("{}{}", node.debug_name, in_expr), i);
                    out << "        " << write_str << "\n";
                    break;
                }
                case PrimitiveType::MACRO_MUX: {
                    const uint32_t w = node.outputs.size();
                    auto [a_expr, next1] = emit_bus_read(node.inputs, 0, w);
                    auto [b_expr, next2] = emit_bus_read(node.inputs, next1, w);
                    auto [sel_expr, _] = emit_bus_read(node.inputs, next2, 1);

                    std::string sel = clean_expr(sel_expr);
                    std::string expr;
                    if (sel == "0ULL") expr = a_expr;
                    else if (sel == "1ULL") expr = b_expr;
                    else expr = std::format("{} ? {} : {}", sel_expr, b_expr, a_expr);

                    auto [write_str, _2] = emit_bus_write(node.outputs, 0, w, expr, i);
                    out << "        " << write_str << "\n";
                    break;
                }
                case PrimitiveType::MACRO_DMUX: {
                    const uint32_t w = node.inputs.size() - 1;
                    auto [in_expr, next1] = emit_bus_read(node.inputs, 0, w);
                    auto [sel_expr, _] = emit_bus_read(node.inputs, next1, 1);

                    std::string sel = clean_expr(sel_expr);
                    std::string a_expr = (sel == "1ULL") ? "0ULL" : (sel == "0ULL" ? in_expr : std::format("{} ? 0ULL : {}", sel_expr, in_expr));
                    std::string b_expr = (sel == "0ULL") ? "0ULL" : (sel == "1ULL" ? in_expr : std::format("{} ? {} : 0ULL", sel_expr, in_expr));

                    auto [write_a, next_out] = emit_bus_write(node.outputs, 0, w, a_expr, i);
                    auto [write_b, _2] = emit_bus_write(node.outputs, next_out, w, b_expr, i);
                    out << "        " << write_a << write_b << "\n";
                    break;
                }
                case PrimitiveType::DFF:
                case PrimitiveType::PERIPHERAL:
                case PrimitiveType::DEAD:
                default: break;
            }
        }
        out << "    }\n\n";
    }

    static void emit_sequential(std::ofstream& out, const CompilerGraph& graph) {
        out << "    void update_sequential() {\n";

        // RAM updates (safe, as they evaluate the currently finalized combinatorial state before modifying anything sequentially)
        for (size_t i = 0; i < graph.nodes.size(); ++i) {
            if (graph.nodes[i].is_dead || graph.nodes[i].type != PrimitiveType::RAM) continue;

            auto [d_expr, next_idx] = emit_bus_read(graph.nodes[i].inputs, 0, graph.nodes[i].args[1]);
            auto [a_expr, next_idx2] = emit_bus_read(graph.nodes[i].inputs, next_idx, graph.nodes[i].args[0]);
            auto [load_expr, _] = emit_bus_read(graph.nodes[i].inputs, next_idx2, 1);

            out << "        if (" << load_expr << ") mem_" << i << "[" << a_expr << "] = " << d_expr << ";\n";
        }

        // Identify physical flip-flops
        std::vector<std::pair<NetID, NetID>> dffs;
        for (const auto& node : graph.nodes) {
            if (node.type == PrimitiveType::DFF && !node.is_dead) {
                // Iterate across the full generic width of the macro
                for (size_t k = 0; k < node.outputs.size(); ++k) {
                    if (node.inputs[k] != node.outputs[k])
                        dffs.emplace_back(node.inputs[k], node.outputs[k]);
                }
            }
        }

        std::vector<std::pair<NetID, NetID>> scalars;
        std::vector<std::string> vector_writes;

        // Emits safely buffered reads for vector DFFs to prevent shift-register race conditions
        for (size_t i = 0; i < dffs.size(); ) {
            size_t j = i + 1;
            int64_t in_step = 0, out_step = 0;

            if (j < dffs.size()) {
                in_step = static_cast<int64_t>(dffs[j].first) - static_cast<int64_t>(dffs[i].first);
                out_step = static_cast<int64_t>(dffs[j].second) - static_cast<int64_t>(dffs[i].second);
                while (j < dffs.size() && j - i < 64 &&
                       static_cast<int64_t>(dffs[j].first) - static_cast<int64_t>(dffs[j-1].first) == in_step &&
                       static_cast<int64_t>(dffs[j].second) - static_cast<int64_t>(dffs[j-1].second) == out_step) {
                    j++;
                }
            }

            if (j - i >= 3) {
                out << std::format("        const uint64_t vec_dff_{} = read_bus<{}, {}, {}>();\n", i, j - i, in_step, dffs[i].first);
                vector_writes.push_back(std::format("        write_bus<{}, {}, {}>({});\n", j - i, out_step, dffs[i].second, std::format("vec_dff_{}", i)));
                i = j;
            } else {
                scalars.push_back(dffs[i]);
                i++;
            }
        }

        for (size_t i = 0; i < scalars.size(); ++i) out << std::format("        const uint64_t dff_{0} = (state[{1} / 64] >> ({1} % 64)) & 1ULL;\n", i, scalars[i].first);
        for (size_t i = 0; i < scalars.size(); ++i) out << std::format("        state[{0} / 64] = (state[{0} / 64] & ~(1ULL << ({0} % 64))) | (dff_{1} << ({0} % 64));\n", scalars[i].second, i);
        for (const auto& w : vector_writes) out << w;

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
        out << "    void load_rom(const char* tag, const char* rom_path) override { chip.load_rom(tag ? tag : \"\", rom_path ? rom_path : \"\"); }\n";
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

        emit_class_header(out_file, chip_name, graph);
        emit_bus_helpers(out_file);
        emit_memory_blocks(out_file, graph);
        emit_peripherals(out_file, chip_name, graph, builder);
        emit_constructor(out_file, chip_name, graph);
        emit_rom_loader(out_file, graph);
        emit_io_api(out_file, graph);
        emit_combinational(out_file, graph);
        emit_sequential(out_file, graph);
        emit_dynamic_wrapper(out_file, chip_name, graph, bp);

        out_file << "#endif // " << chip_name << "_H\n";
    }
};