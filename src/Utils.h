#pragma once
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <system_error>
#include <unordered_map>


namespace Paths {
    constexpr auto CONFIG_FILE = "config.txt";
    constexpr auto KEYS_FILE = "keys.const";

    constexpr auto PROJECTS_DIR = "res/projects/";
    constexpr auto GLOBAL_DIR = "res/global/";
    constexpr auto CPP_DIR = "res/cpp/";

    constexpr auto TST_DIR = "testing/tst";
    constexpr auto CMP_DIR = "testing/cmp";
    constexpr auto TEST_TMP_DIR = "testing/tmp/";
}

namespace Constants {
    constexpr uint32_t MAX_BUS_WIDTH = 64; // DO NOT CHANGE
}

namespace Utils {
    inline std::string normalize_working_dir(const char* argv0) {
        const std::filesystem::path exe_dir = std::filesystem::weakly_canonical(std::filesystem::path(argv0)).parent_path();
        std::filesystem::path root_dir = exe_dir;
        while (!root_dir.empty() && root_dir != root_dir.parent_path()) {
            if (std::filesystem::exists(root_dir / "CMakeLists.txt")) break;
            root_dir = root_dir.parent_path();
        }
        std::filesystem::current_path(root_dir);
        return exe_dir.generic_string();
    }

    inline void confirm_resource_dirs() {
        std::filesystem::create_directories(Paths::PROJECTS_DIR);
        std::filesystem::create_directories(Paths::GLOBAL_DIR);
        std::filesystem::create_directories(Paths::CPP_DIR);

        std::filesystem::create_directories(Paths::TST_DIR);
        std::filesystem::create_directories(Paths::CMP_DIR);
        std::filesystem::create_directories(Paths::TEST_TMP_DIR);
    }

    inline std::string resolve_file(const std::string &target, const std::vector<std::string> &search_paths) {
        for (const std::string &root : search_paths) {
            if (!std::filesystem::exists(root)) continue;
            std::error_code ec;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec)) {
                if (ec) break;
                if (entry.is_regular_file() && entry.path().filename() == target)
                    return entry.path().generic_string();
            }
        }
        throw std::runtime_error(std::string("\n[CRITICAL ERROR] Could not locate '") + target + "' in any search paths\n");
    }

    inline std::string find_keys_file(const std::vector<std::string>& search_paths) {
        return resolve_file(Paths::KEYS_FILE, search_paths);
    }

    // Checks if 'child' is a subdirectory of 'parent'
    inline bool path_contains(const std::filesystem::path& parent, const std::filesystem::path& child) {
        auto p_norm = std::filesystem::absolute(parent).lexically_normal();
        auto c_norm = std::filesystem::absolute(child).lexically_normal();
        auto [p_it, c_it] = std::mismatch(p_norm.begin(), p_norm.end(), c_norm.begin());
        return p_it == p_norm.end();
    }

    // Deduplicates search paths ('parent' + 'parent/child' = 'parent')
    inline void add_unique_search_path(std::vector<std::string>& paths, const std::string& new_path_str) {
        const std::filesystem::path new_path = new_path_str;

        for (auto it = paths.begin(); it != paths.end(); ) {
            std::filesystem::path existing = *it;

            if (path_contains(existing, new_path)) return;

            if (path_contains(new_path, existing)) it = paths.erase(it);
            else ++it;
        }
        paths.push_back(new_path_str);
    }

    // Check a pre-calculated max dependency time against a target file
    inline bool is_outdated(const std::filesystem::file_time_type& source_time, const std::filesystem::path& target) {
        std::error_code ec;
        const auto target_time = std::filesystem::last_write_time(target, ec);
        return ec || source_time > target_time;
    }

    // Check a source file against a target file directly
    inline bool is_outdated(const std::filesystem::path& source, const std::filesystem::path& target) {
        std::error_code ec;
        const auto source_time = std::filesystem::last_write_time(source, ec);
        if (ec) return true; // If source is missing/error, trigger build to log the error properly
        return is_outdated(source_time, target);
    }
}

struct Config {
    std::string active_config;
    std::string target_chip;

    double target_hz = 0.0;
    double target_fps = 30.0;
    bool max_speed = true;

    std::unordered_map<std::string, std::string> rom_map;
    std::string script_path;

    std::vector<std::string> search_paths;
    std::unordered_map<std::string, std::string> pin_actions;

    bool run_tests = false;


    static Config load() {
        std::ifstream file(Paths::CONFIG_FILE);
        if (!file.is_open()) {
            std::string abs_path = std::filesystem::absolute(Paths::CONFIG_FILE).string();
            throw std::runtime_error(std::format("\n[CRITICAL ERROR] Configuration file '{}' not found\nExpected absolute location:\n -> {}\n", Paths::CONFIG_FILE, abs_path));
        }

        Config config;
        Utils::add_unique_search_path(config.search_paths, Paths::GLOBAL_DIR);

        // Defer ROM resolution until all SEARCH_PATHs are processed
        std::vector<std::pair<std::string, std::string>> pending_roms;

        std::string current_block;
        std::string line;

        while (std::getline(file, line)) {
            if (const size_t pos = line.find("//"); pos != std::string::npos) line.resize(pos);
            std::erase_if(line, [](const unsigned char c) { return std::isspace(c); });
            if (line.empty()) continue;

            // Block tracking
            if (line.back() == '{') {
                current_block = line.substr(0, line.size() - 1);
                continue;
            }
            if (line == "}") {
                current_block.clear();
                continue;
            }

            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                const std::string key = line.substr(0, eq);
                const std::string val = line.substr(eq + 1);

                // Parse CONFIG first if it's at the global scope
                if (current_block.empty() && key == "CONFIG") {
                    config.active_config = val;
                    continue;
                }

                // Only process global settings or settings matching the active config block
                if (current_block.empty() || current_block == config.active_config) {
                    if (key == "TARGET_CHIP") config.target_chip = val;
                    else if (key == "CLOCK_SPEED") config.target_hz = std::stod(val);
                    else if (key == "TARGET_FPS") config.target_fps = std::stod(val);
                    else if (key == "MAX_SPEED") config.max_speed = val == "true";
                    else if (key == "RUN_TESTS") config.run_tests = val == "true";
                    else if (key == "SCRIPT") config.script_path = Utils::resolve_file(val, config.search_paths);
                    else if (key == "LOAD_ROM") {
                        const size_t colon = val.find(':');
                        if (colon != std::string::npos) pending_roms.emplace_back(val.substr(0, colon), val.substr(colon + 1));
                    } else if (key == "SEARCH_PATH")
                        Utils::add_unique_search_path(config.search_paths, Paths::PROJECTS_DIR + val);
                    else if (key == "PIN_ACTION") {
                        const size_t colon = val.find(':');
                        if (colon != std::string::npos) config.pin_actions[val.substr(0, colon)] = val.substr(colon + 1);
                    }
                }
            }
        }

        if (config.active_config.empty())
            throw std::runtime_error(std::format("[CRITICAL ERROR] '{}' is missing a global 'CONFIG = <name>' declaration", Paths::CONFIG_FILE));

        if (config.target_chip.empty() && config.script_path.empty())
            throw std::runtime_error(std::format("[CRITICAL ERROR] '{}' block '{}' is missing TARGET_CHIP or SCRIPT", Paths::CONFIG_FILE, config.active_config));

        for (const auto& [tag, file_name] : pending_roms)
            config.rom_map[tag] = Utils::resolve_file(file_name, config.search_paths);

        return config;
    }
};