#include "cli/CliParse.hpp"
#include "cli/FrontendSelection.hpp"
#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include <iostream>

int main(int argc, char** argv) {
    auto parsed = cch::cli::parse_args(argc, argv);
    if (!parsed) {
        // Parse errors carry the full help text in detail.
        const auto& error = parsed.error();
        std::cerr << (error.detail.empty() ? error.message : error.detail) << '\n';
        return 2;
    }
    cch::cli::CliConfig config = std::move(*parsed);
    if (config.help) {
        std::cout << config.help_text;
        return 0;
    }

    const auto environment = cch::cli::detect_frontend_environment();
    if (auto frontend = cch::cli::select_frontend(config, environment); !frontend) {
        const auto& error = frontend.error();
        std::cerr << error.message;
        if (!error.detail.empty() && error.detail != error.message) {
            std::cerr << ": " << error.detail;
        }
        std::cerr << '\n';
        return 2;
    } else {
        return cch::cli::run_async_cli(config, *frontend, environment);
    }
}
