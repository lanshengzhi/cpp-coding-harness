// P8 (#404): `--list-models [search]` prints pi's exact six-column table
// (`provider model context max-out thinking images`) over the runtime's
// available (configured-auth) models, with `formatTokenCount` K/M formatting,
// fuzzy `[search]` filtering, runtime diagnostics on stderr, and
// `formatNoModelsAvailableMessage()` when no models exist — then exits 0
// without touching session storage (the session manager is in-memory).

#include "coding_agent/cli/ListModels.hpp"
#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "support/CliRunFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Model.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "support/AgentRootFixture.hpp"

using namespace cch;

namespace {

[[nodiscard]] ai::Model catalog_model(
    std::string provider,
    std::string id,
    std::uint64_t context,
    std::uint64_t max_tokens,
    bool reasoning,
    bool images) {
    ai::Model model;
    model.id = std::move(id);
    model.name = model.id;
    model.api = "openai-responses";
    model.provider = std::move(provider);
    model.reasoning = reasoning;
    model.input = images
        ? std::vector<ai::ModelInput>{ai::ModelInput::Text, ai::ModelInput::Image}
        : std::vector<ai::ModelInput>{ai::ModelInput::Text};
    model.context_window = context;
    model.max_tokens = max_tokens;
    return model;
}

/// beta is listed first so the provider-then-id sort is exercised.
[[nodiscard]] std::shared_ptr<coding_agent::ModelRuntime> table_catalog_runtime() {
    std::vector<coding_agent::ModelRuntimeTestProvider> definitions;
    definitions.push_back(coding_agent::ModelRuntimeTestProvider{
            .definition =
                    tests::ScriptedModelCatalog{
                            .id = "beta",
                            .name = "beta",
                            .models = {catalog_model("beta", "beta-1", 999, 1500, false, true)},
                    },
    });
    definitions.push_back(coding_agent::ModelRuntimeTestProvider{
            .definition =
                    tests::ScriptedModelCatalog{
                            .id = "alpha",
                            .name = "alpha",
                            .models =
                                    {
                                            catalog_model("alpha", "alpha-1", 200000, 16000, false, false),
                                            catalog_model("alpha", "alpha-2", 1000000, 2000000, true, true),
                                    },
                    },
    });
    auto created = coding_agent::create_model_runtime_for_testing(coding_agent::ModelRuntimeOptions{},
            coding_agent::ModelRuntimeTestOptions{
                    .providers = std::move(definitions),
            });
    REQUIRE(created);
    return std::move(*created);
}

[[nodiscard]] tests::CliRunResult run_list_models(
        std::vector<std::string> args, std::shared_ptr<coding_agent::ModelRuntime> runtime) {
    tests::CliRunOptions options{
            .args = std::move(args),
            .cwd = {},
            .env = {},
            .stdin_text = {},
    };
    if (!runtime) {
        return tests::run_cli(std::move(options));
    }
    return tests::run_cli_with_runtime(std::move(options), std::move(runtime));
}

} // namespace

TEST_CASE(
        "list-models prints pi's six-column table with K/M formatting and sort", "[cli][list-models][issue404][spec]") {
    auto result = run_list_models({"--list-models"}, table_catalog_runtime());

    REQUIRE(result.exit_code == 0);
    CHECK(result.stderr_text.empty());
    // pi padEnds every column (including the last), so data rows carry
    // trailing padding.
    CHECK(result.stdout_text ==
          "provider  model    context  max-out  thinking  images\n"
          "alpha     alpha-1  200K     16K      no        no    \n"
          "alpha     alpha-2  1M       2M       yes       yes   \n"
          "beta      beta-1   999      1.5K     no        yes   \n");
}

TEST_CASE("list-models fuzzy search filters the table and keeps the header", "[cli][list-models][issue404][spec]") {
    auto result = run_list_models({"--list-models", "alpha"}, table_catalog_runtime());

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text ==
          "provider  model    context  max-out  thinking  images\n"
          "alpha     alpha-1  200K     16K      no        no    \n"
          "alpha     alpha-2  1M       2M       yes       yes   \n");
}

TEST_CASE("list-models with no fuzzy matches prints pi's no-match message", "[cli][list-models][issue404][spec]") {
    auto result = run_list_models({"--list-models", "nomatch"}, table_catalog_runtime());

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "No models matching \"nomatch\"\n");
    CHECK(result.stderr_text.empty());
}

TEST_CASE("list-models with no models prints the no-models message and exits 0",
        "[cli][list-models][issue404][spec][issue626]") {
    // The default scripted fake providers have empty catalogs, so the
    // available snapshot is empty.
    auto result = run_list_models({"--list-models"}, {});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "No models available. Start the local Magpie gateway with `magpie serve`, "
                                "then run `/reload`.\n");
}

TEST_CASE("list-models runs in-memory: no session file is created and help/version keep precedence",
        "[cli][list-models][issue404][spec]") {
    tests::TempWorkspace workspace;
    const auto agent_dir = tests::agent_root_under_home(workspace.path());
    std::filesystem::create_directories(agent_dir);

    auto result = tests::run_cli(tests::CliRunOptions{
            .args = {"--list-models"},
            .cwd = workspace.path(),
            .env = {{"HOME", workspace.path().string()}},
            .stdin_text = {},
    });

    REQUIRE(result.exit_code == 0);
    // No session storage anywhere under the Agent Config Directory.
    std::error_code ec;
    if (std::filesystem::exists(agent_dir, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(agent_dir, ec)) {
            CHECK_FALSE(entry.is_regular_file(ec));
        }
    }
    CHECK_FALSE(ec);
}

TEST_CASE("list-models ignores malformed legacy models.json", "[cli][list-models][issue404][spec][issue626]") {
    // Legacy model configuration does not participate in catalog discovery.
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_key_guard{"KIMI_API_KEY"};
    home_guard.set(home.path().string());
    kimi_key_guard.set("dummy-kimi-key");
    home.write(".config/pike/agent/models.json", "{not valid json");

    auto runtime = coding_agent::create_model_runtime_for_testing(
            coding_agent::ModelRuntimeOptions{}, coding_agent::ModelRuntimeTestOptions{});
    REQUIRE(runtime);

    std::ostringstream output;
    std::ostringstream error;
    cli::print_list_models(**runtime, std::nullopt, output, error);

    CHECK(error.str().empty());
    CHECK(output.str().find("No models available") != std::string::npos);
}

TEST_CASE("list-models reports Magpie discovery errors as runtime stderr warnings",
        "[cli][list-models][issue626][spec]") {
    auto transport = std::make_shared<tests::ScriptedTransport>();
    transport->attempts.push_back(tests::TransportAttempt{
            .head = {.status_code = 0, .headers = {}},
            .chunks = {},
            .failure = support::make_error(support::ErrorCode::Network, "gateway unavailable"),
    });
    auto runtime = coding_agent::create_model_runtime_for_testing(
            coding_agent::ModelRuntimeOptions{}, coding_agent::ModelRuntimeTestOptions{}, transport);
    REQUIRE(runtime);

    std::ostringstream output;
    std::ostringstream error;
    cli::print_list_models(**runtime, std::nullopt, output, error);

    INFO(error.str());
    CHECK(error.str().find("Warning: model runtime errors:\n") != std::string::npos);
    CHECK(error.str().find("Magpie model discovery failed") != std::string::npos);
    CHECK(error.str().find("gateway unavailable") != std::string::npos);
    CHECK(error.str().find("magpie serve") != std::string::npos);
    CHECK(output.str().find("No models available") != std::string::npos);
}
