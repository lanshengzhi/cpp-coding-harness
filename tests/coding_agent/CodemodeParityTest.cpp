// Spec #882, ticket #885: the codemode parity differential baseline. The
// frozen `pi-v1.0.4` evidence bundle (`fixtures/pi-ai/v1.0.4/mcp-codemode/`,
// captured from pi `7c10bd43`) is the acceptance authority (ADR 0066 ruling 3)
// for the model-facing `codemode` tool definition, its grammar variant, and its
// single `code` argument. The description's text follows the selected
// pi-v1.3.0 baseline (#1118): the frozen
// `fixtures/pi-v1.3.0/audit-1053/application-catalog.json` empty-catalog
// scenario is the authority for it, because v1.3.0 rewrote the intro and
// globals block.
//
// Removal evidence: #870's `.pi/codemode` declaration face is physically
// deleted. The cheap check is "no declaration symbol exists" — a search that
// passes while a staged `.pi/codemode/*.json` still adds tools. The separation
// case built here stages such a directory and asserts a session's tool surface
// gains nothing from it (and still carries exactly the inline `codemode` tool),
// so the property is the absent surface, not the absent symbol.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include <cch/codemode/Source.hpp>
#include "coding_agent/extensions/codemode/CodemodeTool.hpp"
#include "coding_agent/extensions/codemode/CodemodeToolSource.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/Json.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/TextFile.hpp"

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

namespace codemode = cch::codemode;
namespace extensions = cch::coding_agent::extensions;

[[nodiscard]] std::filesystem::path bundle_root() {
    return std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-ai" / "v1.0.4" / "mcp-codemode";
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] support::JsonValue bundle_tool() {
    auto parsed = support::read_json(read_text(bundle_root() / "codemode-tool.json"));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

/// The pi-v1.3.0 empty-catalog description (`createCodemodeDescription([])`).
[[nodiscard]] std::string v130_empty_description() {
    auto parsed = support::read_json(read_text(std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" /
                                               "audit-1053" / "application-catalog.json"));
    REQUIRE(parsed.has_value());
    for (const auto& scenario : parsed->get_object().at("scenarios").get_array()) {
        if (scenario.get_object().at("name").get_string() ==
                "the empty codemode catalog still declares globals, discovery and the options grammar") {
            return scenario.get_object().at("expected").get_string();
        }
    }
    REQUIRE(false);
    return {};
}

[[nodiscard]] const support::JsonValue& field(const support::JsonValue& object, std::string_view name) {
    const auto& fields = object.get_object();
    const auto it = fields.find(std::string{name});
    REQUIRE(it != fields.end());
    return it->second;
}

[[nodiscard]] std::vector<std::string> string_array(const support::JsonValue& value) {
    std::vector<std::string> result;
    for (const auto& item : value.get_array()) {
        result.push_back(item.get_string());
    }
    return result;
}

/// `JsonValue` has no equality operator; compare through the canonical wire
/// text, which is also what a differential bundle records.
[[nodiscard]] std::string json_text(const support::JsonValue& value) {
    auto written = support::write_json(value);
    REQUIRE(written.has_value());
    return *written;
}

} // namespace

TEST_CASE(
        "the codemode description is the frozen pi-v1.3.0 empty catalog", "[coding_agent][codemode][issue1118][spec]") {
    // #1118: v1.3.0 rewrote DESCRIPTION_INTRO and describeGlobals. The
    // description is the catalog renderer's empty output, not a frozen literal
    // that can drift from the live loadout.
    const auto description = extensions::codemode_tool_definition().description;
    INFO(description);
    CHECK(description == v130_empty_description());
    CHECK(description.find("Nested tools:") == std::string::npos);
}

TEST_CASE("the codemode grammar variant is pi's frozen CODEMODE_SOURCE_GRAMMAR",
        "[coding_agent][codemode][issue885][spec]") {
    const std::string fixture = read_text(bundle_root() / "codemode-source-grammar.lark");

    // Byte-for-byte, including the leading and trailing newline String.raw
    // preserves.
    CHECK(std::string{codemode::kCodemodeSourceGrammar} == fixture);

    const auto tool = bundle_tool();
    const auto& variants = field(field(tool, "constrainedSampling"), "variants");
    CHECK(std::string{codemode::kCodemodeSourceGrammar} == field(variants, "openai_lark").get_string());
}

TEST_CASE("the model-facing codemode tool definition matches the pi-v1.0.4 bundle",
        "[coding_agent][codemode][issue885][spec]") {
    const auto bundle = bundle_tool();
    const auto definition = extensions::codemode_tool_definition();

    CHECK(definition.name == field(bundle, "name").get_string());
    CHECK(definition.name == field(bundle, "toolNameConstant").get_string());
    CHECK(definition.name == extensions::kCodemodeToolName);
    CHECK(std::string{extensions::kCodemodeToolLabel} == field(bundle, "label").get_string());
    CHECK(std::string{extensions::kCodemodeExposure} == field(bundle, "exposure").get_string());
    CHECK(std::string{extensions::kCodemodeStoreEntryType} == field(bundle, "storeEntryType").get_string());
    CHECK(extensions::kCodemodeDefaultInlineBudget ==
            static_cast<std::int64_t>(field(bundle, "defaultInlineBudget").get_number()));
    CHECK(std::string{codemode::kCodemodeOptionsPrefix} == field(bundle, "optionsPrefix").get_string());

    // The description is the pi-v1.3.0 empty-catalog text (see the dedicated
    // case above); the v1.0.4 bundle's own description field predates the
    // v1.3.0 intro/globals rewrite and is not the authority for it.
    CHECK(definition.description == v130_empty_description());

    // The single `code` string argument, verbatim.
    CHECK(json_text(definition.parameters) == json_text(field(bundle, "parameters")));

    // The grammar constraint rides on the definition.
    REQUIRE(definition.constrained_sampling.has_value());
    const auto it = definition.constrained_sampling->variants.find("openai_lark");
    REQUIRE(it != definition.constrained_sampling->variants.end());
    CHECK(it->second == field(field(field(bundle, "constrainedSampling"), "variants"), "openai_lark").get_string());

    // System-prompt contribution.
    CHECK(std::string{extensions::kCodemodePromptSnippet} == field(bundle, "promptSnippet").get_string());
    CHECK(extensions::codemode_prompt_guidelines() == string_array(field(bundle, "promptGuidelines")));
}

TEST_CASE("the codemode tool source contributes exactly pi's one inline tool",
        "[coding_agent][codemode][issue885][spec]") {
    extensions::CodemodeToolSource source{};
    auto tools = source.load_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 1);
    CHECK(tools->front().definition.name == "codemode");
    CHECK(tools->front().prompt_snippet == std::string{extensions::kCodemodePromptSnippet});
}

TEST_CASE("a stray .pi/codemode declaration adds nothing to the session tool surface",
        "[coding_agent][codemode][issue885][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    // The removed #870 declaration face: a `.pi/codemode` directory a project
    // might still carry from before the rollback.
    workspace.write(".pi/codemode/stale.json",
            R"({"name":"stale_declared","description":"Should never appear.","source":"stale.js"})");
    workspace.write(".pi/codemode/stale.js", "return 1;");

    tests::RuntimeFixture runtime;
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.request_model = tests::scripted_request_model("fake", "fake-model");
    options.execution_runtime_target = runtime.make_target();
    auto models = tests::models_from_provider(tests::make_scripted_fake_provider());
    auto created = runtime.run(coding_agent::create_agent_session_async(
            std::move(options), std::nullopt, tests::cli_fake_overrides(std::move(models))));
    REQUIRE(created.has_value());
    auto session = std::move(created->session);

    const auto& names = session->snapshot().agent_state.active_tool_names;
    // The single inline tool is registered but not declared: pi registers
    // codemode with `defaultActive: false` (#884), so the model does not see
    // it until activation names it.
    CHECK(std::ranges::find(names, "codemode") == names.end());
    // ...and the deleted declaration face contributes nothing.
    CHECK(std::ranges::find(names, "stale_declared") == names.end());
    CHECK(std::ranges::find(names, "stale") == names.end());

    session->close();
}

// ---------------------------------------------------------------------------
// #1193: the guest-module load boundary's independent frozen capture
// (audit-1193/sandbox-create.json). The C++ Application source holder keeps
// pi's laziness — a session that never calls codemode never touches the
// guest — while the public create validates eagerly, surfacing a missing
// guest as the tool call's explicit error.
// ---------------------------------------------------------------------------

TEST_CASE("the codemode tool source never touches the guest until a script runs",
        "[coding_agent][codemode][issue1193][spec]") {
    const auto artifact_path =
            std::filesystem::path{CCH_SOURCE_DIR} / "fixtures" / "pi-v1.3.0" / "audit-1193" / "sandbox-create.json";
    auto artifact = support::read_json(tests::read_text_file(artifact_path));
    REQUIRE(artifact.has_value());
    const support::JsonValue* never = nullptr;
    for (const auto& candidate : artifact->get_object().at("scenarios").get_array()) {
        if (candidate.get_object().at("name").get_string() == "a sandbox that never executes never touches the guest")
            never = &candidate;
    }
    REQUIRE(never != nullptr);

    // The guest path does not exist; loading the tool source still succeeds,
    // so a session assembling this source never observes the missing guest.
    const auto missing = codemode::default_codemode_guest_wasm_path() / "definitely-missing.wasm";
    extensions::CodemodeToolSource source{missing};
    auto tools = source.load_tools();
    REQUIRE(tools.has_value());
    REQUIRE(tools->size() == 1);
    CHECK(tools->front().definition.name == "codemode");

    // The frozen observation this boundary honors: pi constructs cleanly and
    // never loads when nothing executes.
    const auto& expected = never->get_object().at("expected").get_object();
    CHECK(expected.at("constructionOk").get_boolean());
    CHECK(expected.at("closeOk").get_boolean());
    CHECK(expected.at("loadAttempts").get_number() == 0);

    // The first script surfaces the explicit create error — never a silent
    // success, never a deferred failure inside the run.
    extensions::ExtensionToolContext context;
    context.call_id = "call_1";
    auto executed = tools->front().context_execute(
            support::JsonValue{support::JsonValue::object_t{{"code", support::JsonValue{"return 1;"}}}},
            std::move(context),
            std::stop_token{});
    auto outcome = tests::run_async_result(std::move(executed));
    // The lazy holder refuses at its ensure boundary: the failure is the
    // call's explicit error, never a silent success and never an assembly
    // failure for a session that did not run a script.
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().message.find("definitely-missing.wasm") != std::string::npos);
}
