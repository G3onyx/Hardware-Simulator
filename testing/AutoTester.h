#pragma once
#include <vector>
#include <string>

struct TestTarget {
    std::string test_name;
    std::string chip_name;
};

class AutoTester {
public:
    static int generate_and_run_suite(std::vector<std::string>& search_paths, const std::string& exe_dir);
};