#pragma once

#include "cli/CliConfig.hpp"
#include "cli/FrontendSelection.hpp"

#include <cch/ai/Models.hpp>

#include <iosfwd>
#include <memory>

namespace cch::cli {

/// The CLI's three process streams, injected so the entry chain is testable
/// in-process. The executable passes std::cin/std::cout/std::cerr.
struct CliStreams {
    std::istream& input;
    std::ostream& output;
    std::ostream& error;
};

/// Runtime options for the CLI entry chain. The environment observes the
/// process streams (TTY detection); the models seam is test-only and injects
/// the deterministic fake provider catalog (the surface the `--fake` flag
/// used to drive).
struct CliRuntimeOptions {
    FrontendEnvironment environment{};
    bool environment_explicit{false};
    std::shared_ptr<ai::Models> models;
};

[[nodiscard]] int run_async_cli(
    const CliConfig& config,
    Frontend frontend,
    CliStreams streams,
    FrontendEnvironment environment,
    std::shared_ptr<ai::Models> models = {});

/// The CLI entry chain (bootstrap parse -> help/version -> frontend selection
/// -> runtime), shared by main() and the in-process CLI test seam.
[[nodiscard]] int run_cli_entry(
    int argc,
    char** argv,
    CliStreams streams,
    CliRuntimeOptions options = {});

} // namespace cch::cli
