#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <format>
#include <vector>
#include "DynamicChipManager.h"
#include "Utils.h"

struct VirtualRAM {
    std::vector<uint8_t> memory;
    std::string addr_pin;
    std::string din_pin;
    std::string dout_pin;
    std::string we_pin;
    bool we_active = false;
};

class ScriptEngine {
public:
    static void execute(DynamicChipManager& manager, Config& config, const std::string& exe_dir) {
        if (config.script_path.empty()) return;

        std::ifstream file(config.script_path);
        if (!file.is_open()) {
            std::cerr << "[ScriptEngine] Could not open script: " << config.script_path << "\n";
            return;
        }

        std::string line;
        uint32_t line_num = 0;
        IDynamicChip* chip = nullptr;
        VirtualRAM v_ram;
        std::string clock_pin;

        std::cout << "\n--- STARTING SCRIPT: " << config.script_path << " ---\n";

        // Virtual RAM evaluation lambda
        auto eval_ram = [&] {
            if (v_ram.memory.empty() || !chip) return;
            const uint64_t addr = chip->get_pin(v_ram.addr_pin.c_str());
            const uint64_t we = chip->get_pin(v_ram.we_pin.c_str());

            if ((we != 0) == v_ram.we_active) {
                // Write Mode
                const uint64_t data = chip->get_pin(v_ram.dout_pin.c_str());
                if (addr < v_ram.memory.size()) v_ram.memory[addr] = data & 0xFF;
            } else {
                // Read Mode
                const uint64_t data = addr < v_ram.memory.size() ? v_ram.memory[addr] : 0;
                chip->set_pin(v_ram.din_pin.c_str(), data);
            }
        };

        while (std::getline(file, line)) {
            line_num++;
            if (const size_t pos = line.find("//"); pos != std::string::npos) line.resize(pos);
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            if (line.empty()) continue;

            std::stringstream ss(line);
            std::string cmd;
            ss >> cmd;

            try {
                if (cmd == "target") {
                    std::string target_chip;
                    ss >> target_chip;
                    std::cout << "[ScriptEngine] Target set to: " << target_chip << "\n";
                    manager.request_load(target_chip, exe_dir, config.search_paths);
                    while (manager.is_busy()) {
                        manager.check_and_swap(target_chip);
                        std::this_thread::yield();
                    }
                    chip = manager.get_chip();
                    if (!chip) throw std::runtime_error("Failed to compile target chip.");
                }
                else if (cmd == "ram") {
                    size_t size;
                    ss >> size >> v_ram.addr_pin >> v_ram.din_pin >> v_ram.dout_pin >> v_ram.we_pin >> v_ram.we_active;
                    v_ram.memory.resize(size, 0);
                    std::cout << "[ScriptEngine] Virtual RAM configured: " << size << " bytes\n";
                }
                else if (cmd == "load_rom") {
                    std::string path;
                    ss >> path;
                    if (path.front() == '"') path = path.substr(1, path.length() - 2);
                    std::string full_path = Utils::resolve_file(path, config.search_paths);

                    std::ifstream rom(full_path, std::ios::binary | std::ios::ate);
                    if (!rom) throw std::runtime_error("Could not open ROM: " + full_path);

                    const size_t size = rom.tellg();
                    rom.seekg(0, std::ios::beg);
                    rom.read(reinterpret_cast<char*>(v_ram.memory.data()), std::min(size, v_ram.memory.size()));
                    std::cout << "[ScriptEngine] Loaded ROM: " << path << " (" << size << " bytes)\n";
                }
                else if (cmd == "clock") {
                    ss >> clock_pin;
                    std::cout << "[ScriptEngine] Clock pin set to: " << clock_pin << "\n";
                }
                else if (cmd == "set") {
                    std::string pin; uint64_t val;
                    ss >> pin >> val;
                    if (chip) chip->set_pin(pin.c_str(), val);
                }
                else if (cmd == "tick") {
                    uint64_t count = 1;
                    if (!ss.eof()) ss >> count;
                    if (!chip) throw std::runtime_error("No target chip loaded.");

                    for (uint64_t i = 0; i < count; ++i) {
                        if (!clock_pin.empty()) {
                            // Phase 1: Clock Low
                            chip->set_pin(clock_pin.c_str(), 0);
                            chip->update_combinational();
                            eval_ram(); // CPU outputs address; RAM reacts
                            chip->update_combinational(); // Propagate RAM's data into CPU
                            chip->update_sequential(); // Latch Phase 1 registers

                            // Phase 2: Clock High
                            chip->set_pin(clock_pin.c_str(), 1);
                            chip->update_combinational();
                            eval_ram();
                            chip->update_combinational();
                            chip->update_sequential(); // Latch Phase 2 registers
                        } else {
                            eval_ram();
                            chip->emulate_frame(1, 0, 0);
                        }
                    }
                    if (count > 100) std::cout << "  Ticked " << count << " cycle(s)\n";
                }
                else if (cmd == "print") {
                    std::string pin;
                    ss >> pin;
                    if (chip) {
                        chip->update_combinational();
                        const uint64_t val = chip->get_pin(pin.c_str());
                        std::cout << "  Print: " << pin << " = " << val << " (0x" << std::hex << val << std::dec << ")\n";
                    }
                }
                else if (cmd == "assert") {
                    std::string pin; uint64_t expected;
                    ss >> pin >> expected;
                    if (chip) {
                        chip->update_combinational();
                        const uint64_t actual = chip->get_pin(pin.c_str());
                        if (actual != expected) {
                            std::cerr << std::format("[ASSERT FAILED] Line {}: Expected {} = {}, got {}\n",
                                                     line_num, pin, expected, actual);
                            return; // Halt script
                        }
                    }
                }
                else {
                    std::cerr << "[ScriptEngine] Unknown command on line " << line_num << ": " << cmd << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "[ScriptEngine] Error on line " << line_num << ": " << e.what() << "\n";
                return;
            }
        }
        std::cout << "--- SCRIPT COMPLETE ---\n\n";
    }
};