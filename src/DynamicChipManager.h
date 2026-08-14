#pragma once
#include <windows.h>
#include <string>
#include <iostream>
#include <filesystem>
#include <future>
#include <format>
#include "IDynamicChip.h"
#include "Utils.h"

class DynamicChipManager {
    typedef IDynamicChip* (*CreateChipFunc)();
    typedef void (*DestroyChipFunc)(IDynamicChip*);

    HMODULE current_dll = nullptr;
    IDynamicChip* current_instance = nullptr;
    DestroyChipFunc current_destroy_fn = nullptr;
    std::string current_temp_dll;

    std::future<bool> compile_task;
    bool is_compiling = false;

    static bool compile_chip_process(const std::string& chip_name, const std::string& exe_dir, const std::vector<std::string>& search_paths) {
        std::string transpiler_path = (std::filesystem::path(exe_dir) / "Transpiler.exe").generic_string();

        std::string transpile_cmd = std::format(R"(""{}" "{}")", transpiler_path, chip_name);
        for (const auto& path : search_paths) {
            std::string safe_path = std::filesystem::path(path).lexically_normal().generic_string();
            transpile_cmd += std::format(R"( "{}")", safe_path);
        }
        transpile_cmd += "\"";

        if (std::system(transpile_cmd.c_str()) != 0) return false;

        const std::string h_path = std::format("{}{}.h", Paths::CPP_DIR, chip_name);
        const std::string dll_path = std::format("{}{}.dll", Paths::CPP_DIR, chip_name);

        if (!Utils::is_outdated(h_path, dll_path)) {
            std::cout << "[Manager] " << chip_name << ".dll is up to date. Skipping.\n";
            return true;
        }

        const std::string compile_cmd = std::format(
            R"(g++ -shared -DBUILD_DYNAMIC_LIBRARY -std=c++20 -O3 -march=native -static-libgcc -static-libstdc++ -Isrc -x c++ -o "{}" "{}")",
            dll_path, h_path
        );

        std::cout << "[Manager] Compiling " << chip_name << ".dll..." << std::flush;

        const bool success = std::system(compile_cmd.c_str()) == 0;

        if (success) std::cout << " COMPLETE\n";
        else std::cout << " FAILED\n";

        return success;
    }

public:
    std::string target_name;

    DynamicChipManager() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path(), ec)) {
            if (entry.is_regular_file()) {
                const std::string filename = entry.path().filename().string();
                if (filename.starts_with("sim_") && filename.ends_with(".dll"))
                    std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    ~DynamicChipManager() { unload_current(); }

    [[nodiscard]] bool is_busy() const { return is_compiling; }

    void request_load(const std::string& chip_name, const std::string& exe_dir, const std::vector<std::string>& search_paths) {
        if (is_compiling) return;
        is_compiling = true;
        target_name = chip_name;

        compile_task = std::async(std::launch::async, [this, chip_name, exe_dir, search_paths] {
            return compile_chip_process(chip_name, exe_dir, search_paths);
        });
    }

    bool check_and_swap(const std::string& chip_name) {
        if (!is_compiling) return false;

        if (compile_task.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;

        is_compiling = false;
        if (!compile_task.get()) {
            std::cerr << "[Manager] Build failed\n";
            return false;
        }

        auto time_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const std::string base_dll = std::format("{}{}.dll", Paths::CPP_DIR, chip_name);
        const std::filesystem::path shadow_path = std::filesystem::temp_directory_path() / std::format("sim_{}_{}.dll", chip_name, time_id);
        const std::string shadow_dll = shadow_path.string();

        try {
            std::filesystem::copy_file(base_dll, shadow_dll, std::filesystem::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            std::cerr << "[Manager] Failed to create shadow copy: " << e.what() << "\n";
            return false;
        }

        unload_current();

        current_dll = LoadLibraryW(shadow_path.wstring().c_str());
        if (!current_dll) {
            std::cerr << "[Manager] CRITICAL: LoadLibraryW failed. Windows error code: " << GetLastError() << "\n";
            std::filesystem::remove(shadow_dll);
            return false;
        }

        auto create_fn = reinterpret_cast<CreateChipFunc>(GetProcAddress(current_dll, "create_chip"));
        if (!create_fn) create_fn = reinterpret_cast<CreateChipFunc>(GetProcAddress(current_dll, "_create_chip"));

        current_destroy_fn = reinterpret_cast<DestroyChipFunc>(GetProcAddress(current_dll, "destroy_chip"));
        if (!current_destroy_fn) current_destroy_fn = reinterpret_cast<DestroyChipFunc>(GetProcAddress(current_dll, "_destroy_chip"));

        if (!create_fn || !current_destroy_fn) {
            std::cerr << "[Manager] CRITICAL: GetProcAddress failed to find exported functions\n";
            unload_current();
            std::filesystem::remove(shadow_dll);
            return false;
        }

        current_instance = create_fn();
        current_temp_dll = shadow_dll;
        
        std::cout << "[Manager] Successfully loaded " << chip_name << "\n";
        return true;
    }

    [[nodiscard]] IDynamicChip* get_chip() const { return current_instance; } // NOLINT

private:
    void unload_current() {
        if (current_instance && current_destroy_fn) current_destroy_fn(current_instance); // NOLINT
        if (current_dll) FreeLibrary(current_dll); // NOLINT
        if (!current_temp_dll.empty()) {
            std::error_code ec;
            std::filesystem::remove(current_temp_dll, ec);
        }
        
        current_instance = nullptr;
        current_dll = nullptr;
        current_destroy_fn = nullptr;
    }
};