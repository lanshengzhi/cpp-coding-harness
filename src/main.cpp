#include "cli/CliParse.hpp"
#include "cli/CliPreflight.hpp"
#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include <iostream>

int main(int argc, char** argv) {
    auto parsed = cch::cli::parse_args(argc, argv);
    if (!parsed) {
        cch::cli::print_error(parsed.error());
        return 2;
    }
    cch::cli::CliConfig config = std::move(*parsed);
    if (config.help) {
        std::cout << config.help_text;
        return 0;
    }
    auto validation = cch::cli::preflight_cli_config(config);
    if (!validation) {
        cch::cli::print_error(validation.error());
        return 2;
    }

    auto workspace_validation = cch::cli::validate_workspace(config.workspace);
    if (!workspace_validation) {
        cch::cli::print_error(workspace_validation.error());
        return 2;
    }
    config.workspace = cch::cli::canonical_workspace(config.workspace);

    return cch::cli::run_async_cli(cch::cli::to_runtime_config(std::move(config)));
}
