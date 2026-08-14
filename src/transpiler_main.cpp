//
// Created by georg on 21/07/2026.
//

#include <iostream>
#include <vector>
#include "Utils.h"
#include "Transpiler.h"

int main(const int argc, char *argv[]) {
    Utils::normalize_working_dir(argv[0]);

    if (argc < 2) {
        std::cerr << "Usage: Transpiler <TargetChip> [SearchPaths...]" << std::endl;
        return 1;
    }

    try {
        std::vector<std::string> search_paths;
        for (int i = 2; i < argc; ++i)
            search_paths.emplace_back(argv[i]);

        Transpiler::run_pipeline(argv[0], argv[1], search_paths, Paths::CPP_DIR);
    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL TRANSPILER ERROR] " << e.what() << "\n" << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n[FATAL TRANSPILER ERROR] Unknown exception\n" << std::endl;
        return 1;
    }

    return 0;
}