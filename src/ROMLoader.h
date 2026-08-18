#pragma once
#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <format>
#include <cctype>
#include "IDynamicChip.h"
#include "Utils.h"

class RomLoader {
public:
    static void load_all(const Config& config, IDynamicChip* chip) {
        if (!chip) return;

        for (const auto& [tag, path] : config.rom_map) {
            if (path.ends_with(".hack"))
                load_hack(tag, path, chip);
            else
                load_binary(tag, path, chip);
        }
    }

private:
    static void load_hack(const std::string& tag, const std::string& path, IDynamicChip* chip) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "[ROM Loader] Warning: Could not open " << path << "\n";
            return;
        }

        std::string line;
        size_t addr = 0;
        while (std::getline(file, line)) {
            // Trim comments and whitespace
            if (const size_t pos = line.find("//"); pos != std::string::npos) line = line.substr(0, pos);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
            size_t start = 0;
            while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) start++;
            if (start >= line.size()) continue;
            line = line.substr(start);
            
            if (line.empty() || (line[0] != '0' && line[0] != '1')) continue;

            // Parse generic length binary string
            uint64_t val = 0;
            for (size_t i = 0; i < line.length() && i < 64; ++i) {
                if (line[i] == '1') val |= 1ULL << (line.length() - 1 - i);
            }

            // Inject via set_pin
            try {
                chip->set_pin(std::format("{}[{}]", tag, addr).c_str(), val);
                addr++;
            } catch (...) {
                std::cout << "[ROM Loader] Reached capacity of memory '" << tag << "' at " << addr << " words.\n";
                break;
            }
        }
        std::cout << "[ROM Loader] Loaded " << addr << " words into '" << tag << "' from " << path << "\n";
    }

    static void load_binary(const std::string& tag, const std::string& path, IDynamicChip* chip) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "[ROM Loader] Warning: Could not open binary file " << path << "\n";
            return;
        }

        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            std::cerr << "[ROM Loader] Warning: Failed to read " << path << "\n";
            return;
        }

        // Write sequential bytes into the hardware
        size_t addr = 0;
        for (; addr < buffer.size(); ++addr) {
            try {
                chip->set_pin(std::format("{}[{}]", tag, addr).c_str(), buffer[addr]);
            } catch (...) {
                std::cout << "[ROM Loader] Reached capacity of memory '" << tag << "' at " << addr << " bytes.\n";
                break;
            }
        }
        std::cout << "[ROM Loader] Loaded " << addr << " bytes into '" << tag << "' from " << path << "\n";
    }
};