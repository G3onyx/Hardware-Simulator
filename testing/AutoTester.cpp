#include "AutoTester.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <algorithm>
#include "../src/Utils.h"
#include "../src/Transpiler.h"

int AutoTester::generate_and_run_suite(std::vector<std::string>& search_paths, const std::string& exe_dir) {
    std::cout << "\n=== GENERATING TEST SUITE ===\n" << std::flush;

    Utils::add_unique_search_path(search_paths, Paths::TST_DIR);
    Utils::add_unique_search_path(search_paths, Paths::CMP_DIR);

    std::vector<TestTarget> test_targets;
    std::vector<std::string> unique_chips;

    // Scan for all .tst files and parse them to find the target .hdl file
    for (const auto& path : search_paths) {
        if (!std::filesystem::exists(path)) continue;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".tst") {
                std::string test_name = entry.path().stem().string();
                std::ifstream tst_file(entry.path());
                std::string line, chip_name;

                while (std::getline(tst_file, line)) {
                    size_t load_pos = line.find("load ");
                    if (load_pos != std::string::npos) {
                        size_t hdl_pos = line.find(".hdl", load_pos);
                        if (hdl_pos != std::string::npos) {
                            size_t start = load_pos + 5;
                            chip_name = line.substr(start, hdl_pos - start);
                            chip_name.erase(0, chip_name.find_first_not_of(" \t"));
                            chip_name.erase(chip_name.find_last_not_of(" \t") + 1);
                            break;
                        }
                    }
                }

                if (!chip_name.empty()) {
                    test_targets.push_back({test_name, chip_name});
                    if (std::ranges::find(unique_chips, chip_name) == unique_chips.end())
                        unique_chips.push_back(chip_name);
                }
            }
        }
    }

    if (test_targets.empty()) {
        std::cout << "[AutoTester] No valid .tst files found. Skipping tests.\n" << std::flush;
        return 0;
    }

    // Transpile all unique chips discovered in the test scripts
    for (const std::string& chip_name : unique_chips) {
        try {
            Transpiler::run_pipeline(exe_dir, chip_name, search_paths, Paths::TEST_TMP_DIR);
        } catch (const std::exception& e) {
            std::cerr << "[Warning] Skipping dependencies for " << chip_name << " (Transpile failed: " << e.what() << ")\n" << std::flush;
        }
    }

    // Dynamically write the test_suite.cpp file
    const std::string runner_path = std::string(Paths::TEST_TMP_DIR) + "test_suite.cpp";
    std::ofstream out(runner_path);

    out << "#include <vector>\n#include <string>\n#include <iostream>\n";
    out << "#include \"../Tester.h\"\n\n";

    for (const auto& chip : unique_chips) out << "#include \"" << chip << ".h\"\n";

    out << "\nint main() {\n";
    out << "    std::vector<std::string> search_paths = {";
    for (size_t i = 0; i < search_paths.size(); ++i) {
        std::string safe_path = search_paths[i];
        while(safe_path.find('\\') != std::string::npos) safe_path.replace(safe_path.find('\\'), 1, "/");
        out << "\"" << safe_path << "\"" << (i < search_paths.size() - 1 ? ", " : "");
    }
    out << "};\n\n";

    for (const auto& target : test_targets) {
        out << "    try {\n";
        out << "        " << target.chip_name << " chip_inst;\n";
        out << "        Tester::run_script(chip_inst, \"" << target.test_name << "\", search_paths);\n";
        out << "    } catch (const std::exception& e) { std::cout << \"  -> FAILED: \" << e.what() << \"\\n\"; }\n";
    }

    out << "    return 0;\n}\n";
    out.close();

    #ifdef _WIN32
        std::string exec_path = std::string(Paths::TEST_TMP_DIR) + "test_suite_exec.exe";
        std::string exec_cmd = exec_path;
        std::ranges::replace(exec_cmd, '/', '\\');
    #else
        std::string exec_path = std::string(Paths::TEST_TMP_DIR) + "test_suite_exec";
        std::string exec_cmd = "./" + exec_path;
    #endif

    // Only trigger the G++ runtime execution if the script or the transpiled chips have genuinely changed!
    bool needs_compile = Utils::is_outdated(runner_path, exec_path);
    if (!needs_compile) {
        for (const auto& chip : unique_chips) {
            if (Utils::is_outdated(std::string(Paths::TEST_TMP_DIR) + chip + ".h", exec_path)) {
                needs_compile = true; break;
            }
        }
    }

    if (needs_compile) {
        std::cout << "\n[AutoTester] Compiling test suite...\n" << std::flush;
        std::string compile_cmd = "g++ -std=c++20 " + runner_path + " -Isrc -o " + exec_path;
        if (std::system(compile_cmd.c_str()) != 0) {
            std::cerr << "[AutoTester] Compilation failed\n";
            return 1;
        }
    } else {
        std::cout << "\n[AutoTester] Test suite executable up to date.\n" << std::flush;
    }

    std::cout << "[AutoTester] Running tests...\n" << std::flush;
    std::system(exec_cmd.c_str());

    return 0;
}