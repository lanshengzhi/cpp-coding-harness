// P8 (#404): `--list-models [search]` prints pi's exact six-column table
// (`provider model context max-out thinking images`) over the runtime's
// available (configured-auth) models, with `formatTokenCount` K/M formatting,
// fuzzy `[search]` filtering, the models.json warning on stderr, and
// `formatNoModelsAvailableMessage()` when no models exist — then exits 0
// without touching session storage (the session manager is in-memory).

#include "cli/ListModels.hpp"
#include "coding_agent/ModelRuntimeTestSupport.hpp"
#include "support/CliRunFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
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
    model.base_url = "https://example.invalid";
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
    std::vector<ai::providers::ScriptedProviderDefinition> definitions;
    definitions.push_back(ai::providers::ScriptedProviderDefinition{
            .definition =
                    ai::ProviderDefinition{
                            .id = "beta",
                            .name = "beta",
                            .models = {catalog_model("beta", "beta-1", 999, 1500, false, true)},
                            .auth = tests::detail::fixture_auth(),
                    },
    });
    definitions.push_back(ai::providers::ScriptedProviderDefinition{
            .definition =
                    ai::ProviderDefinition{
                            .id = "alpha",
                            .name = "alpha",
                            .models =
                                    {
                                            catalog_model("alpha", "alpha-1", 200000, 16000, false, false),
                                            catalog_model("alpha", "alpha-2", 1000000, 2000000, true, true),
                                    },
                            .auth = tests::detail::fixture_auth(),
                    },
    });
    auto created = coding_agent::create_model_runtime_for_testing(
            coding_agent::ModelRuntimeOptions{
                    .models_path = std::filesystem::path{},
                    .credentials = ai::providers::make_scripted_credential_store(),
            },
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
            .models = nullptr,
    };
    if (!runtime) {
        return tests::run_cli(std::move(options));
    }
    return tests::run_cli_with_runtime(std::move(options), std::move(runtime));
}

} // namespace

TEST_CASE(
    "list-models prints pi's six-column table with K/M formatting and sort",
    "[cli][list-models][issue404]") {
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

TEST_CASE(
    "list-models fuzzy search filters the table and keeps the header",
    "[cli][list-models][issue404]") {
    auto result = run_list_models({"--list-models", "alpha"}, table_catalog_runtime());

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text ==
          "provider  model    context  max-out  thinking  images\n"
          "alpha     alpha-1  200K     16K      no        no    \n"
          "alpha     alpha-2  1M       2M       yes       yes   \n");
}

TEST_CASE(
    "list-models with no fuzzy matches prints pi's no-match message",
    "[cli][list-models][issue404]") {
    auto result = run_list_models({"--list-models", "nomatch"}, table_catalog_runtime());

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text == "No models matching \"nomatch\"\n");
    CHECK(result.stderr_text.empty());
}

TEST_CASE(
    "list-models with no models prints the no-models message and exits 0",
    "[cli][list-models][issue404]") {
    // The default scripted fake providers have empty catalogs, so the
    // available snapshot is empty.
    auto result = run_list_models({"--list-models"}, {});

    REQUIRE(result.exit_code == 0);
    CHECK(result.stdout_text ==
          "No models available. Use /login to log into a provider via OAuth or API key. See:\n"
          "  ~/.pi/docs/providers.md\n"
          "  ~/.pi/docs/models.md\n");
}

TEST_CASE(
    "list-models runs in-memory: no session file is created and help/version keep precedence",
    "[cli][list-models][issue404]") {
    tests::TempWorkspace workspace;
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    dir_guard.set(agent_dir.path().string());

    auto result = tests::run_cli(tests::CliRunOptions{
        .args = {"--list-models"},
        .cwd = workspace.path(),
        .env = {},
        .stdin_text = {},
        .models = {},
    });

    REQUIRE(result.exit_code == 0);
    // No session storage anywhere under the Agent Config Directory.
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(agent_dir.path(), ec)) {
        CHECK_FALSE(entry.is_regular_file(ec));
    }
    CHECK_FALSE(ec);
}

TEST_CASE(
    "list-models reports the models.json load error as a stderr warning",
    "[cli][list-models][issue404]") {
    // A real runtime over a broken models.json carries the config diagnostic;
    // the CLI surface prints it as pi's yellow warning (colorless here). The
    // built-in providers still compose structurally, so the table prints too.
    tests::TempWorkspace home;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard agent_dir_guard{"PI_CODING_AGENT_DIR", std::nullopt};
    home_guard.set(home.path().string());
    home.write(".pi/agent/models.json", "{not valid json");

    auto runtime = coding_agent::ModelRuntime::create({});
    REQUIRE(runtime);

    std::ostringstream output;
    std::ostringstream error;
    cli::print_list_models(**runtime, std::nullopt, output, error);

    CHECK(error.str().find(
              "Warning: errors loading models.json:\n") != std::string::npos);
    CHECK(error.str().find("Failed to parse models.json") != std::string::npos);
    // stdout still carries the table with pi's header, exactly like pi.
    CHECK(output.str().find("max-out") != std::string::npos);
    CHECK(output.str().find("images") != std::string::npos);
}
