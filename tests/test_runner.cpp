#include "mock/test_framework.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>

int main(int argc, char* argv[]) {
    std::string filter;
    std::string tier_filter;
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            list_only = true;
        } else if (arg.rfind("--filter=", 0) == 0) {
            filter = arg.substr(9);
        } else if (arg.rfind("--tier=", 0) == 0) {
            tier_filter = "Tier" + arg.substr(7);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "RTL8723BE Mock Hardware & 4-Tier Test Runner\n";
            std::cout << "Usage: " << argv[0] << " [options] [filter]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --filter=<substr>   Run only tests matching <substr>\n";
            std::cout << "  --tier=<1|2|3|4>    Run only tests in Tier 1, 2, 3, or 4\n";
            std::cout << "  --list              List all registered tests\n";
            std::cout << "  --help              Show this help message\n";
            return 0;
        } else if (arg[0] != '-') {
            filter = arg;
        }
    }

    const auto& all_tests = rtl_test::TestRegistry::instance().tests();

    if (list_only) {
        std::cout << "Registered Tests (" << all_tests.size() << " total):\n";
        for (const auto& t : all_tests) {
            std::cout << "  [" << t.category << "] " << t.name << "\n";
        }
        return 0;
    }

    std::vector<const rtl_test::TestCase*> matched_tests;
    for (const auto& t : all_tests) {
        if (!tier_filter.empty()) {
            if (t.category.rfind(tier_filter, 0) != 0) {
                continue;
            }
        }
        if (!filter.empty()) {
            std::string full_id = t.category + "." + t.name;
            if (full_id.find(filter) == std::string::npos) {
                continue;
            }
        }
        matched_tests.push_back(&t);
    }

    std::cout << "\033[1;36m========================================================================\033[0m\n";
    std::cout << "\033[1;36m       Realtek RTL8723BE Automated Mock & Protocol Test Suite           \033[0m\n";
    std::cout << "\033[1;36m========================================================================\033[0m\n";
    std::cout << "Running " << matched_tests.size() << " test(s) from " << all_tests.size() << " registered.\n\n";

    size_t passed = 0;
    size_t failed = 0;
    auto suite_start = std::chrono::high_resolution_clock::now();

    for (const auto* t : matched_tests) {
        std::string test_id = t->category + "." + t->name;
        std::cout << "\033[1;33m[ RUN      ]\033[0m " << test_id << "\n";

        auto start_time = std::chrono::high_resolution_clock::now();
        bool ok = false;
        std::string err_msg;

        try {
            t->func();
            ok = true;
        } catch (const rtl_test::TestFailureException& e) {
            err_msg = e.what();
        } catch (const std::exception& e) {
            err_msg = std::string("Unexpected std::exception: ") + e.what();
        } catch (...) {
            err_msg = "Unknown non-standard exception caught";
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        double duration_ms = static_cast<double>(duration_us) / 1000.0;

        if (ok) {
            passed++;
            std::cout << "\033[1;32m[       OK ]\033[0m " << test_id << " ("
                      << std::fixed << std::setprecision(2) << duration_ms << " ms)\n";
        } else {
            failed++;
            std::cout << "\033[1;31m[  FAILED  ]\033[0m " << test_id << " ("
                      << std::fixed << std::setprecision(2) << duration_ms << " ms)\n";
            std::cout << "             \033[0;31mError: " << err_msg << "\033[0m\n";
        }
    }

    auto suite_end = std::chrono::high_resolution_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(suite_end - suite_start).count();

    std::cout << "\n\033[1;36m========================================================================\033[0m\n";
    std::cout << "\033[1;36m                          Test Execution Summary                        \033[0m\n";
    std::cout << "\033[1;36m========================================================================\033[0m\n";
    std::cout << "Total Tests Run:  " << matched_tests.size() << "\n";
    std::cout << "\033[1;32mPassed:           " << passed << "\033[0m\n";
    if (failed > 0) {
        std::cout << "\033[1;31mFailed:           " << failed << "\033[0m\n";
    } else {
        std::cout << "Failed:           0\n";
    }
    std::cout << "Total Duration:   " << total_ms << " ms\n";
    std::cout << "Result:           " << (failed == 0 ? "\033[1;32mALL TESTS PASSED (100%)\033[0m" : "\033[1;31mTEST FAILURES OCCURRED\033[0m") << "\n\n";

    return (failed == 0) ? 0 : 1;
}
