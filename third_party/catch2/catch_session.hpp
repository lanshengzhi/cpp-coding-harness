#pragma once

#include "catch_test_macros.hpp"

#include <iostream>
#include <string>

namespace Catch {
class Session {
public:
    int run(int argc, char** argv) {
        std::string filter;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--list-tests") {
                for (const auto& test : CatchLite::registry()) {
                    std::cout << test.name << ' ' << test.tags << '\n';
                }
                return 0;
            }
            if (!arg.empty() && arg[0] != '-') {
                filter = arg;
            }
        }

        int failures = 0;
        int ran = 0;
        for (const auto& test : CatchLite::registry()) {
            if (!filter.empty() && test.name.find(filter) == std::string::npos && test.tags.find(filter) == std::string::npos) {
                continue;
            }
            ++ran;
            try {
                test.body();
                std::cout << "[pass] " << test.name << '\n';
            } catch (const std::exception& e) {
                ++failures;
                std::cerr << "[fail] " << test.name << ": " << e.what() << '\n';
            } catch (...) {
                ++failures;
                std::cerr << "[fail] " << test.name << ": unknown exception\n";
            }
        }
        std::cout << ran << " test(s), " << failures << " failure(s)\n";
        return failures == 0 ? 0 : 1;
    }
};
} // namespace Catch
