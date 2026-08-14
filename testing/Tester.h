#pragma once
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include "../src/Utils.h"

struct OutputFormat {
    std::string pin_name;
    char type{};
    int pad_left{};
    int width{};
    int pad_right{};
};

class Tester {
    static std::vector<std::string> tokenize(const std::string &script) {
        std::vector<std::string> tokens;
        std::string current;
        bool in_line_comment = false, in_block_comment = false, in_string = false;

        for (size_t i = 0; i < script.length(); ++i) {
            if (in_string) {
                current += script[i];
                if (script[i] == '"') {
                    tokens.push_back(current);
                    current.clear();
                    in_string = false;
                }
                continue;
            }
            if (in_line_comment) {
                if (script[i] == '\n') in_line_comment = false;
                continue;
            }
            if (in_block_comment) {
                if (i + 1 < script.length() && script[i] == '*' && script[i+1] == '/') {
                    in_block_comment = false; i++;
                }
                continue;
            }
            if (i + 1 < script.length() && script[i] == '/' && script[i+1] == '/') {
                in_line_comment = true; i++; continue;
            }
            if (i + 1 < script.length() && script[i] == '/' && script[i+1] == '*') {
                in_block_comment = true; i++; continue;
            }

            const char c = script[i];
            if (c == '"') {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
                current += c;
                in_string = true;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
            } else if (c == '{' || c == '}' || c == ',' || c == ';') {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
                tokens.emplace_back(1, c);
            } else {
                current += c;
            }
        }
        if (!current.empty()) tokens.push_back(current);
        return tokens;
    }

    static uint64_t parse_value(const std::string &val_str) {
        if (val_str.starts_with("%B")) return std::stoull(val_str.substr(2), nullptr, 2);
        if (val_str.starts_with("%X")) return std::stoull(val_str.substr(2), nullptr, 16);
        return static_cast<uint64_t>(std::stoll(val_str));
    }

    static std::string format_pin(const std::string& pin_name, const uint64_t val, const OutputFormat &fmt, const int cycle_count, const bool is_tock, const bool is_error) {
        std::string s;

        if (is_error) {
            s = "ERR";
        } else if (pin_name == "time") {
            s = std::to_string(cycle_count) + (is_tock ? "+" : "");
        } else if (fmt.type == 'B') {
            for (int i = fmt.width - 1; i >= 0; --i) s += ((val >> i) & 1) ? "1" : "0";
        } else if (fmt.type == 'D') {
            auto signed_val = static_cast<int32_t>(val);
            if (val & 0x8000) signed_val |= 0xFFFF0000;
            s = std::to_string(signed_val);
        } else {
            s = std::to_string(val);
        }

        if (s.length() > fmt.width) s = s.substr(s.length() - fmt.width);
        else if (s.length() < fmt.width) {
            if (pin_name == "time" || fmt.type == 'S') s += std::string(fmt.width - s.length(), ' ');
            else s = std::string(fmt.width - s.length(), ' ') + s;
        }

        return std::string(fmt.pad_left, ' ') + s + std::string(fmt.pad_right, ' ');
    }

    static void advance_block(const std::vector<std::string>& tokens, size_t& i, const size_t end) {
        int depth = 1;
        i++; // Skip opening '{'
        while (i < end && depth > 0) {
            if (tokens[i] == "{") depth++;
            else if (tokens[i] == "}") depth--;
            i++;
        }
    }

    static void handle_output_list(const std::vector<std::string>& tokens, size_t& i, const size_t end, std::vector<OutputFormat>& formats, std::ifstream& cmp_file) {
        i++;
        while (i < end && tokens[i] != ";") {
            if (tokens[i] == ",") { i++; continue; }

            const std::string &f_tok = tokens[i];
            size_t pct = f_tok.find('%');
            OutputFormat fmt;

            if (pct != std::string::npos) {
                fmt.pin_name = f_tok.substr(0, pct);
                std::string f_args = f_tok.substr(pct + 1);
                fmt.type = f_args[0];
                sscanf(f_args.c_str() + 1, "%d.%d.%d", &fmt.pad_left, &fmt.width, &fmt.pad_right);
            } else {
                fmt.pin_name = f_tok;
                fmt.type = 'B';
                fmt.pad_left = 1; fmt.width = 1; fmt.pad_right = 1;
            }

            if (fmt.pin_name.ends_with("[]")) fmt.pin_name = fmt.pin_name.substr(0, fmt.pin_name.length() - 2);

            formats.push_back(fmt);
            i++;
        }
        std::string cmp_line;
        std::getline(cmp_file, cmp_line); // Clear the header row
    }

    template <typename ChipT>
    static void handle_output(ChipT &chip, const std::vector<OutputFormat>& formats, std::ifstream& cmp_file, int& line_num, const int cycle_count, const bool is_tock) {
        line_num++;
        std::string generated_row = "|";

        for (const auto &f: formats) {
            uint64_t val = 0;
            bool is_error = false;

            if (f.pin_name != "time") {
                std::string target_pin = f.pin_name;

                try {
                    val = chip.get_pin(target_pin);
                } catch (...) {
                    const size_t bracket = target_pin.find('[');
                    if (bracket != std::string::npos) {
                        std::string base_name = target_pin.substr(0, bracket);
                        try {
                            val = chip.get_pin(base_name);

                            // Official test quirk: asking for ARegister[0] but expecting the full 16-bit decimal
                            if (base_name != "addressM" && base_name != "DRegister") {
                                const size_t end_bracket = target_pin.find(']', bracket);
                                if (end_bracket != std::string::npos) {
                                    const int bit_idx = std::stoi(target_pin.substr(bracket + 1, end_bracket - bracket - 1));
                                    val = (val >> bit_idx) & 1ULL;
                                }
                            }
                        }
                        catch (...) { is_error = true; }
                    } else {
                        is_error = true;
                    }
                }
            }
            generated_row += format_pin(f.pin_name, val, f, cycle_count, is_tock, is_error) + "|";
        }

        std::string cmp_line;
        if (!std::getline(cmp_file, cmp_line)) throw std::runtime_error("Test failed! .cmp file ran out of lines.");
        std::erase(cmp_line, '\r');

        bool match = (generated_row.length() == cmp_line.length());
        if (match) {
            for(size_t c = 0; c < cmp_line.length(); ++c) {
                if (cmp_line[c] != '*' && cmp_line[c] != generated_row[c]) {
                    match = false; break;
                }
            }
        }

        if (!match) {
            std::cout << "\n[TEST FAILED] Mismatch at output line " << line_num << "\n";
            std::cout << "Expected: " << cmp_line << "\n";
            std::cout << "Actual:   " << generated_row << "\n" << std::flush;
            throw std::runtime_error("Verification failed.");
        }
    }

    template <typename ChipT>
    static void execute_tokens(ChipT &chip, const std::vector<std::string>& tokens, size_t& i, const size_t end,
                               std::vector<OutputFormat>& formats, std::ifstream& cmp_file, int& line_num,
                               int& cycle_count, bool& is_tock, const std::vector<std::string>& search_paths) {
        while (i < end) {
            const std::string &cmd = tokens[i];

            if (cmd == "," || cmd == ";") { i++; }
            else if (cmd == "load" || cmd == "compare-to" || cmd == "output-file") { i += 2; }
            else if (cmd.starts_with("ROM") && i + 2 < end && tokens[i + 1] == "load") {
                try { chip.load_rom(tokens[i].c_str(), Utils::resolve_file(tokens[i + 2], search_paths).c_str()); }
                catch (...) { std::cerr << "[Warning] Could not find ROM to inject: " << tokens[i+2] << "\n" << std::flush; }
                i += 3;
            }
            else if (cmd == "echo") {
                i++;
                if (i < end && tokens[i].starts_with("\"")) i++; // Safely skip strings
            }
            else if (cmd == "clear-echo") { i++; }
            else if (cmd == "while") {
                while (i < end && tokens[i] != "{") i++; // Bypass condition
                if (i < end && tokens[i] == "{") advance_block(tokens, i, end); // Safely skip interactive blocks
            }
            else if (cmd == "output-list") { handle_output_list(tokens, i, end, formats, cmp_file); }
            else if (cmd == "set") {
                std::string pin = tokens[i + 1];
                if (pin.ends_with("[]")) pin = pin.substr(0, pin.length() - 2);
                try { chip.set_pin(pin, parse_value(tokens[i + 2])); } catch (...) {}
                i += 3;
            }
            else if (cmd == "eval") { chip.update_combinational(); i++; }
            else if (cmd == "tick") { chip.update_combinational(); is_tock = true; i++; }
            else if (cmd == "tock") {
                chip.update_sequential();
                chip.update_combinational();
                is_tock = false; cycle_count++; i++;
            }
            else if (cmd == "output") {
                handle_output(chip, formats, cmp_file, line_num, cycle_count, is_tock);
                i++;
            }
            else if (cmd == "repeat") {
                const int loops = std::stoi(tokens[i + 1]);
                i += 2;
                if (tokens[i] == "{") i++;

                const size_t block_start = i;
                advance_block(tokens, i, end);
                size_t block_end = i - 1;

                for (int l = 0; l < loops; ++l) {
                    size_t temp_i = block_start;
                    execute_tokens(chip, tokens, temp_i, block_end, formats, cmp_file, line_num, cycle_count, is_tock, search_paths);
                }
            }
            else { i++; }
        }
    }

public:
    template <typename ChipT>
    static void run_script(ChipT &chip, const std::string &test_name, const std::vector<std::string>& search_paths) {
        const std::string tst_path = Utils::resolve_file(test_name + ".tst", search_paths);
        const std::string cmp_path = Utils::resolve_file(test_name + ".cmp", search_paths);

        std::ifstream tst_file(tst_path);
        std::ifstream cmp_file(cmp_path);

        if (!tst_file.is_open()) throw std::runtime_error("Could not open test script: " + tst_path);
        if (!cmp_file.is_open()) throw std::runtime_error("Could not open compare file: " + cmp_path);

        std::string full_script((std::istreambuf_iterator(tst_file)), std::istreambuf_iterator<char>());
        std::vector<std::string> tokens = tokenize(full_script);

        std::vector<OutputFormat> formats;
        int line_num = 0, cycle_count = 0;
        bool is_tock = false;
        size_t index = 0;

        std::cout << "Testing: " << test_name << "... " << std::flush;
        execute_tokens(chip, tokens, index, tokens.size(), formats, cmp_file, line_num, cycle_count, is_tock, search_paths);
        std::cout << "PASSED (" << line_num << " lines matched)\n" << std::flush;
    }
};