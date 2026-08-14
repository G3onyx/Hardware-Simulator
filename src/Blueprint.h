//
// Created by georg on 17/07/2026.
//

#pragma once
#include <ranges>
#include <string>
#include <variant>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <optional>
#include <algorithm>


struct Wire {
    std::string name;
    std::vector<std::string> dims;
};

struct Slice {
    std::string wire_name;
    std::vector<std::string> wire_dims;
    std::string lsb, msb;
    std::string rep_count = "1";
};

struct Literal {
    std::string val;
    std::string rep_count = "1";
};

using Chunk = std::variant<Slice, Literal>;
using Bundle = std::vector<Chunk>;

struct Part {
    std::string type; // Register, CPU, Nand, etc...
    std::vector<std::string> generic_args;
    std::unordered_map<std::string, Bundle> pins;
    std::string tag; // optional tag
};


// TO STRING HELPERS
inline std::string to_string(const Bundle &bundle);

inline std::string to_string(const Chunk &chunk) {
    return std::visit([]<typename T0>(T0 &&arg) -> std::string {
        using T = std::decay_t<T0>;
        std::string s;

        if constexpr (std::is_same_v<T, Literal>) {
            s = arg.val;
            if (arg.rep_count != "1") return arg.rep_count + "{" + s + "}";
            return s;
        }
        else if constexpr (std::is_same_v<T, Slice>) {
            s = arg.wire_name;
            // Add array indices (e.g. RAM[5])
            for (const auto &dim: arg.wire_dims) s += "[" + dim + "]";
            // Add slicing
            if (!arg.msb.empty() || !arg.lsb.empty()) {
                if (arg.msb == arg.lsb) s += "[" + arg.lsb + "]";
                else s += "[" + arg.lsb + ":" + arg.msb + "]";
            }
            if (arg.rep_count != "1") return arg.rep_count + "{" + s + "}";
            return s;
        }
        return "UNKNOWN_CHUNK";
    }, chunk);
}

inline std::string to_string(const Bundle &bundle) {
    if (bundle.empty()) return "_"; // Explicit discard placeholder
    if (bundle.size() == 1) return to_string(bundle[0]);

    std::string res = "{";
    for (size_t i = 0; i < bundle.size(); ++i) {
        res += to_string(bundle[i]);
        if (i < bundle.size() - 1) res += ", ";
    }
    res += "}";
    return res;
}

/**
 * The local structure of a parsed HDL chip
 * Stores all information to expand the chip into a full graph
 */
class Blueprint {
public:
    std::string name;

    std::vector<std::string> generic_params;
    std::unordered_map<std::string, std::vector<std::optional<std::string>>> flags;
    std::unordered_map<std::string, std::string> constants;

    std::unordered_map<std::string, Wire> wires;
    std::vector<std::string> in_wires, out_wires;

    std::vector<Part> parts;

    [[nodiscard]] bool is_internal_wire(const std::string &wire_name) const {
        auto not_in = [](const std::vector<std::string>& vec, const std::string& val) {
            return std::ranges::find(vec, val) == vec.end();
        };
        return not_in(in_wires, wire_name) && not_in(out_wires, wire_name);
    }

    [[nodiscard]] bool is_io_pin(const std::string &net_name) const {
        const std::string prefix = name + ".";
        if (net_name.find(prefix) != 0) return false;
        const std::string pin_name = net_name.substr(prefix.length());
        if (pin_name.find('.') != std::string::npos) return false;

        auto in_vec = [&](const std::vector<std::string>& vec) {
            return std::ranges::find(vec, pin_name) != vec.end();
        };
        return in_vec(in_wires) || in_vec(out_wires);
    }

    void print_debug() const {
        std::cout << "\n=== BLUEPRINT: " << name << " ===\n";

        // Generics
        if (!generic_params.empty()) {
            std::cout << "Generics: <";
            for (size_t i = 0; i < generic_params.size(); ++i) {
                std::cout << generic_params[i] << (i < generic_params.size() - 1 ? ", " : "");
            }
            std::cout << ">\n";
        }

        // Flags
        if (!flags.empty()) {
            std::cout << "Flags: ";
            for (const auto &[f, values]: flags) {
                for (const auto &val: values) {
                    std::cout << "@" << f;
                    if (val) std::cout << "(\"" << *val << "\")";
                    std::cout << " ";
                }
            }
            std::cout << "\n";
        }

        // Constants
        if (!constants.empty()) {
            std::cout << "Constants:\n";
            for (const auto &[c_name, c_val]: constants) {
                std::cout << "  " << c_name << " = " << c_val << "\n";
            }
        }

        // Helper to print wire declarations
        auto print_wire = [this](const std::string &prefix, const std::string &w_name) {
            const auto &[w_id, dims] = wires.at(w_name);
            std::cout << "  " << prefix << " " << w_id;
            for (const auto &dim: dims) std::cout << "[" << dim << "]";
            std::cout << "\n";
        };

        // External Interfaces
        std::cout << "Wires:\n";
        for (const auto &w_name: in_wires) print_wire("IN   ", w_name);
        for (const auto &w_name: out_wires) print_wire("OUT  ", w_name);

        // Internal Nets
        for (const auto &w_name: wires | std::views::keys) {
            if (is_internal_wire(w_name))
                print_wire("NET  ", w_name);
        }

        // Parts
        std::cout << "Parts (" << parts.size() << "):\n";
        for (size_t i = 0; i < parts.size(); ++i) {
            const auto &[type, generic_args, pins, _] = parts[i]; // TODO: Print tag

            std::cout << "  [" << i << "] " << type;

            if (!generic_args.empty()) {
                std::cout << "<";
                for (size_t j = 0; j < generic_args.size(); ++j) {
                    std::cout << generic_args[j] << (j < generic_args.size() - 1 ? ", " : "");
                }
                std::cout << ">";
            }
            std::cout << "\n";

            for (const auto &[pin_name, bundle]: pins) {
                std::cout << "      ." << pin_name << " = ";
                if (bundle.empty()) {
                    std::cout << "_ (unconnected)\n";
                } else {
                    std::cout << to_string(bundle) << "\n";
                }
            }
        }
        std::cout << "==========================\n\n";
    }
};
