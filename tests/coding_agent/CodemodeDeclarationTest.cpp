// Spec #865 codemode slice (#870): project-local codemode declarations and
// source loading through the #867 Extension Tool Source seam. The declared
// script tool becomes a real Agent Tool — visible and listable in the session
// — but its execute must NOT run the script: it reports an explicit "not wired
// yet" error until the sandbox slice (#874). The fixtures under
// fixtures/codemode/ pin the format; every invalid-declaration case here is one
// error class and asserts the load fails rather than being skipped.
//
// The separation case the cheap visibility check lets through: a session can
// list a declared tool while execute silently succeeds (or runs the script).
// The tests therefore pair "visible in active_tool_names" with "a call returns
// the explicit error and never script content".

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/extensions/ExtensionToolRegistry.hpp"
#include "coding_agent/extensions/ExtensionToolSource.hpp"
#include "coding_agent/extensions/codemode/CodemodeDeclaration.hpp"
#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>
#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using coding_agent::extensions::CodemodeDeclaration;
using coding_agent::extensions::CodemodeToolSource;

[[nodiscard]] std::filesystem::path fixture_root() {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "codemode";
}

[[nodiscard]] std::string read_fixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// Materialize one fixture file under a temp workspace so a session sees the
/// production `<workspace>/.pi/codemode/` layout.
void stage_fixture(const std::filesystem::path& source, tests::TempWorkspace& workspace, std::string relative) {
    workspace.write(std::move(relative), read_fixture(source));
}

void stage_valid_declarations(tests::TempWorkspace& workspace) {
    const auto root = fixture_root() / "declarations";
    for (const std::string_view name : {"summarize_repo", "triage_issues"}) {
        stage_fixture(root / (std::string{name} + ".json"), workspace, ".pi/codemode/" + std::string{name} + ".json");
        stage_fixture(root / (std::string{name} + ".js"), workspace, ".pi/codemode/" + std::string{name} + ".js");
    }
}

[[nodiscard]] std::vector<CodemodeDeclaration> load_declarations(const std::filesystem::path& directory) {
    auto loaded = coding_agent::extensions::load_codemode_declarations(directory);
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
}

[[nodiscard]] support::Error load_error(const std::filesystem::path& declaration_file) {
    auto loaded = coding_agent::extensions::load_codemode_declaration(declaration_file);
    REQUIRE_FALSE(loaded.has_value());
    return loaded.error();
}

[[nodiscard]] bool session_exposes_tool(const coding_agent::AgentSession& session, std::string_view name) {
    const auto& names = session.snapshot().agent_state.active_tool_names;
    return std::ranges::find(names, name) != names.end();
}

[[nodiscard]] std::optional<std::string> tool_result_text(
        const coding_agent::AgentSession& session, std::string_view name) {
    for (const auto& message : session.snapshot().agent_state.messages) {
        const auto* result = std::get_if<ai::ToolResultMessage>(&message);
        if (result != nullptr && result->tool_name == name) {
            return ai::text_from_content(result->content);
        }
    }
    return std::nullopt;
}

/// Scripted provider that calls one tool with `{"path": "README.md"}` on its
/// first request and then answers a plain text turn, so a prompt exercises the
/// declared tool through the ordinary executor path without live providers.
class CodemodeCallProvider final : public tests::ScriptedProvider {
public:
    explicit CodemodeCallProvider(std::string tool_name) : ScriptedProvider("fake"), tool_name_(std::move(tool_name)) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        const int request = request_count_++;
        const std::string tool_name = tool_name_;
        return ai::detail::make_model_stream(
                [model = std::move(model), request, tool_name](ai::AssistantEventSink sink) mutable
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "codemode-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (request == 0) {
                        round.content = {ai::text_content("calling the declared tool")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_codemode",
                                .name = tool_name,
                                .arguments = support::JsonValue::object_t{{"path", "README.md"}},
                                .raw_arguments = R"({"path":"README.md"})",
                                .thought_signature = std::nullopt,
                                .arguments_valid = true,
                                .argument_error = std::nullopt,
                        });
                    } else {
                        round.content = {ai::text_content("done")};
                        round.stop_reason = ai::AssistantStopReason::Stop;
                    }
                    if (sink) {
                        if (auto emitted = sink(ai::AssistantStartEvent{.partial = round}); !emitted) {
                            co_return std::unexpected(emitted.error());
                        }
                    }
                    co_return round;
                });
    }

private:
    std::string tool_name_;
    int request_count_{0};
};

[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> make_codemode_session(tests::RuntimeFixture& runtime,
        const tests::TempWorkspace& workspace,
        std::shared_ptr<tests::ScriptedProvider> provider,
        std::vector<std::unique_ptr<coding_agent::extensions::ExtensionToolSource>> sources) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    options.extension_tool_sources = std::move(sources);
    auto models = tests::models_from_provider(std::move(provider));
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    return std::move(created->session);
}

} // namespace

TEST_CASE("a workspace without a codemode directory declares no script tools",
        "[coding_agent][codemode][issue870][spec]") {
    tests::TempWorkspace workspace;
    CodemodeToolSource source{workspace.path()};

    auto tools = source.load_tools();

    REQUIRE(tools.has_value());
    CHECK(tools->empty());
}

TEST_CASE("a valid codemode declaration loads its descriptor, schema, and parsed source",
        "[coding_agent][codemode][issue870][spec]") {
    const auto declarations = load_declarations(fixture_root() / "declarations");

    REQUIRE(declarations.size() == 2);
    // File-name order: `summarize_repo` before `triage_issues`.
    const auto& summarize = declarations.at(0);
    CHECK(summarize.name == "summarize_repo");
    CHECK(summarize.description == "Summarize the repository's top-level files.");
    REQUIRE(summarize.input_schema.has_value());
    CHECK(summarize.input_schema->get_object().contains("properties"));
    CHECK(summarize.source_path.filename() == "summarize_repo.js");
    CHECK(summarize.source.options.max_output_tokens == std::nullopt);
    CHECK(summarize.source.options.timeout_ms == std::nullopt);
    // A declaration without an options line keeps its source verbatim.
    CHECK(summarize.source.code == read_fixture(fixture_root() / "declarations" / "summarize_repo.js"));

    const auto& triage = declarations.at(1);
    CHECK(triage.name == "triage_issues");
    CHECK_FALSE(triage.input_schema.has_value());
    REQUIRE(triage.source.options.max_output_tokens.has_value());
    CHECK(*triage.source.options.max_output_tokens == 2000);
    REQUIRE(triage.source.options.timeout_ms.has_value());
    CHECK(*triage.source.options.timeout_ms == 30000);
}

TEST_CASE("the codemode source parser splits pi's options line and preserves line numbers",
        "[coding_agent][codemode][issue870][spec]") {
    const auto parsed =
            coding_agent::extensions::parse_codemode_source("// @options: {\"max_output_tokens\": 1000}\nreturn 1;\n");

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->options.max_output_tokens.has_value());
    CHECK(*parsed->options.max_output_tokens == 1000);
    // The options line becomes an empty line, so the body still starts on line 2.
    CHECK(parsed->code == "\nreturn 1;\n");

    const auto plain = coding_agent::extensions::parse_codemode_source("return 2;");
    REQUIRE(plain.has_value());
    CHECK(plain->code == "return 2;");
    CHECK(plain->options.max_output_tokens == std::nullopt);
}

TEST_CASE(
        "the codemode source parser rejects an out-of-range options line", "[coding_agent][codemode][issue870][spec]") {
    const auto parsed =
            coding_agent::extensions::parse_codemode_source("// @options: {\"timeout_ms\": 0}\nreturn 1;\n");

    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().code == support::ErrorCode::Validation);
}

TEST_CASE("the loader reports a malformed codemode declaration instead of skipping it",
        "[coding_agent][codemode][issue870][spec]") {
    const auto error = load_error(fixture_root() / "invalid" / "malformed.json");

    CHECK(error.code == support::ErrorCode::Validation);
    CHECK(error.message.find("not valid JSON") != std::string::npos);
}

TEST_CASE("the loader rejects a codemode declaration missing a required field",
        "[coding_agent][codemode][issue870][spec]") {
    CHECK(load_error(fixture_root() / "invalid" / "missing_name.json").message.find("`name`") != std::string::npos);
    CHECK(load_error(fixture_root() / "invalid" / "missing_description.json").message.find("`description`") !=
            std::string::npos);
    CHECK(load_error(fixture_root() / "invalid" / "missing_source.json").message.find("`source`") != std::string::npos);
}

TEST_CASE(
        "the loader rejects a codemode declaration with an unknown field", "[coding_agent][codemode][issue870][spec]") {
    const auto error = load_error(fixture_root() / "invalid" / "unknown_field.json");

    CHECK(error.code == support::ErrorCode::Validation);
    CHECK(error.message.find("unknown field `extra`") != std::string::npos);
}

TEST_CASE("the loader rejects a codemode input schema that is not an object",
        "[coding_agent][codemode][issue870][spec]") {
    const auto error = load_error(fixture_root() / "invalid" / "bad_input_schema.json");

    CHECK(error.code == support::ErrorCode::Validation);
    CHECK(error.message.find("`inputSchema`") != std::string::npos);
}

TEST_CASE("the loader rejects an unreadable codemode source file", "[coding_agent][codemode][issue870][spec]") {
    const auto error = load_error(fixture_root() / "invalid" / "missing_source_file.json");

    CHECK(error.code == support::ErrorCode::Validation);
    CHECK(error.message.find("could not be read") != std::string::npos);
}

TEST_CASE("the loader rejects an empty codemode source", "[coding_agent][codemode][issue870][spec]") {
    const auto error = load_error(fixture_root() / "invalid" / "empty_source.json");

    CHECK(error.code == support::ErrorCode::Validation);
    CHECK(error.message.find("empty.js") != std::string::npos);
    CHECK(error.detail.find("non-empty") != std::string::npos);
}

TEST_CASE("the loader rejects a codemode source with an invalid options line",
        "[coding_agent][codemode][issue870][spec]") {
    const auto error = load_error(fixture_root() / "invalid" / "bad_options.json");

    CHECK(error.code == support::ErrorCode::Validation);
    CHECK(error.message.find("invalid") != std::string::npos);
}

TEST_CASE("loading a codemode directory aborts on an invalid declaration rather than skipping it",
        "[coding_agent][codemode][issue870][spec]") {
    tests::TempWorkspace workspace;
    // A valid declaration sorts first; the invalid one must still abort the load.
    stage_fixture(fixture_root() / "declarations" / "summarize_repo.json", workspace, ".pi/codemode/aaa_valid.json");
    stage_fixture(fixture_root() / "declarations" / "summarize_repo.js", workspace, ".pi/codemode/summarize_repo.js");
    stage_fixture(fixture_root() / "invalid" / "malformed.json", workspace, ".pi/codemode/zzz_broken.json");

    auto loaded = coding_agent::extensions::load_codemode_declarations(
            coding_agent::extensions::codemode_declaration_directory(workspace.path()));

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == support::ErrorCode::Validation);
    CHECK(loaded.error().message.find("zzz_broken.json") != std::string::npos);
}

TEST_CASE("the loader refuses two declarations that declare the same codemode tool name",
        "[coding_agent][codemode][issue870][spec]") {
    const auto duplicate_fixtures = fixture_root() / "duplicate";
    // Point the source at the duplicate fixtures by staging them at the
    // production location under a temp workspace.
    tests::TempWorkspace temp;
    for (const std::string_view name : {"alpha.json", "beta.json", "dup.js"}) {
        stage_fixture(duplicate_fixtures / std::string{name}, temp, ".pi/codemode/" + std::string{name});
    }
    CodemodeToolSource staged{temp.path()};

    std::array<coding_agent::extensions::ExtensionToolSource*, 1> sources{&staged};
    coding_agent::extensions::ExtensionToolRegistry registry;
    auto loaded = coding_agent::extensions::load_extension_tools(registry, sources);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == support::ErrorCode::Validation);
    CHECK(loaded.error().message.find("dup_tool") != std::string::npos);
    // The first registration survives; the duplicate never silently replaces it.
    CHECK(registry.size() == 1);
}

TEST_CASE("a declared codemode tool's execute reports the explicit not-yet-executable error",
        "[coding_agent][codemode][issue870][spec]") {
    tests::TempWorkspace workspace;
    stage_valid_declarations(workspace);
    CodemodeToolSource source{workspace.path()};

    auto tools = source.load_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 2);
    auto tool = std::ranges::find_if(
            *tools, [](const auto& candidate) { return candidate.definition.name == "summarize_repo"; });
    REQUIRE(tool != tools->end());
    // The declaration's schema becomes the Tool's parameters unchanged.
    CHECK(tool->definition.parameters.get_object().contains("properties"));
    CHECK(tool->concurrency == agent::ToolConcurrency::Exclusive);

    auto executed = tool->execute(support::JsonValue::object_t{{"path", "README.md"}}, std::stop_token{});
    auto outcome = tests::run_async_result(std::move(executed));

    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().code == support::ErrorCode::Validation);
    CHECK(outcome.error().message.find("codemode execution is not wired until #874") != std::string::npos);
}

TEST_CASE("a declared codemode tool is visible in the session and its call returns the explicit error",
        "[coding_agent][codemode][issue870][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;
    stage_valid_declarations(workspace);

    std::vector<std::unique_ptr<coding_agent::extensions::ExtensionToolSource>> sources;
    sources.push_back(std::make_unique<CodemodeToolSource>(workspace.path()));
    auto session = make_codemode_session(
            runtime, workspace, std::make_shared<CodemodeCallProvider>("summarize_repo"), std::move(sources));

    // Visible and listable: the declared script tool is on the Agent's surface.
    CHECK(session_exposes_tool(*session, "summarize_repo"));
    CHECK(session_exposes_tool(*session, "triage_issues"));

    // Callable only in the sense of discovery: the call reaches execute, which
    // reports the explicit error, and never the script's result.
    REQUIRE(tests::run_awaitable(runtime, session->prompt("summarize the repo")).has_value());
    const auto result = tool_result_text(*session, "summarize_repo");
    REQUIRE(result.has_value());
    CHECK(result->find("#874") != std::string::npos);
    CHECK(result->find("loaded for discovery only") != std::string::npos);

    session->close();
}

TEST_CASE("a declared codemode tool is visible only when the codemode source is assembled",
        "[coding_agent][codemode][issue870][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;
    stage_valid_declarations(workspace);

    // The declarations exist on disk and load, but a session assembled without
    // the source does not expose them: visibility is registration at assembly,
    // not the presence of the declaration.
    auto without =
            make_codemode_session(runtime, workspace, std::make_shared<CodemodeCallProvider>("summarize_repo"), {});
    CHECK_FALSE(session_exposes_tool(*without, "summarize_repo"));
    without->close();

    std::vector<std::unique_ptr<coding_agent::extensions::ExtensionToolSource>> sources;
    sources.push_back(std::make_unique<CodemodeToolSource>(workspace.path()));
    auto with = make_codemode_session(
            runtime, workspace, std::make_shared<CodemodeCallProvider>("summarize_repo"), std::move(sources));
    CHECK(session_exposes_tool(*with, "summarize_repo"));
    with->close();
}
