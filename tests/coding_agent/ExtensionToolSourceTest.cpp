// Spec #865 first slice (#867): the Minimal Extension Tool Source. The one new
// seam is a loader/runner/registry that converts an extension-provided tool
// into a `cch::agent::Tool` and feeds the existing ToolRegistry at Session
// assembly. The stub source here stands in for the later MCP and codemode
// sources; the scripted fake provider serves the model, so no live keys or
// network are used.
//
// The acceptance case pairs "the tool is loaded/registered" with "the tool is
// on the session's surface": an extension tool that is loaded but never
// registered at assembly must stay invisible to the session, so the tests
// cannot pass on registration alone.

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/extensions/ExtensionToolRegistry.hpp"
#include "coding_agent/extensions/ExtensionToolSource.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/ToolExecutionComponent.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/FakeTool.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/TempWorkspace.hpp"
#include "support/ToolRendererFixture.hpp"

#include <cch/agent/AgentTool.hpp>
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
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

/// A deterministic Extension Tool Source: it contributes one named tool whose
/// execute records its invocation and returns a fixed text result. This is the
/// stub every case is proven against; the MCP and codemode slices replace it
/// behind the same `ExtensionToolSource` interface.
class StubExtensionToolSource final : public coding_agent::extensions::ExtensionToolSource {
public:
    StubExtensionToolSource(std::string tool_name, std::shared_ptr<std::size_t> call_count)
        : tool_name_(std::move(tool_name)), call_count_(std::move(call_count)) {}

    [[nodiscard]] support::Expected<std::vector<coding_agent::extensions::ExtensionTool>> load_tools() override {
        coding_agent::extensions::ExtensionTool tool;
        tool.definition.name = tool_name_;
        tool.definition.description = "Stub extension tool";
        tool.definition.parameters = support::JsonValue::object_t{
                {"type", "object"},
                {"properties", support::JsonValue::object_t{}},
                {"additionalProperties", false},
        };
        tool.concurrency = agent::ToolConcurrency::ParallelSafe;
        tool.prompt_snippet = "Run the stub extension tool";
        auto call_count = call_count_;
        tool.execute = [call_count](support::JsonValue,
                               std::stop_token) -> support::AsyncResult<coding_agent::extensions::ExtensionToolResult> {
            if (call_count) {
                ++*call_count;
            }
            return support::AsyncResult<coding_agent::extensions::ExtensionToolResult>{
                    support::Expected<coding_agent::extensions::ExtensionToolResult>{
                            coding_agent::extensions::ExtensionToolResult{
                                    .content = {ai::text_content("stub_ext: ok")},
                                    .details = std::nullopt,
                                    .is_error = false,
                            }}};
        };
        std::vector<coding_agent::extensions::ExtensionTool> tools;
        tools.push_back(std::move(tool));
        return tools;
    }

private:
    std::string tool_name_;
    std::shared_ptr<std::size_t> call_count_;
};

/// Scripted provider that calls the named tool on its first request and then
/// answers a plain text turn, so one prompt exercises the full extension tool
/// round through the ordinary executor path.
class ExtensionToolRoundProvider final : public tests::ScriptedProvider {
public:
    explicit ExtensionToolRoundProvider(std::string tool_name)
        : ScriptedProvider("fake"), tool_name_(std::move(tool_name)) {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext, coding_agent::ModelRuntimeTestStreamOptions) override {
        const int request = request_count_++;
        const std::string tool_name = tool_name_;
        return ai::detail::make_model_stream(
                [model = std::move(model), request, tool_name](ai::AssistantEventSink sink) mutable
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    ai::AssistantMessage round;
                    round.provider = "extension-fake";
                    round.api = "fake";
                    round.model = model.id;
                    if (request == 0) {
                        round.content = {ai::text_content("calling the stub tool")};
                        round.stop_reason = ai::AssistantStopReason::ToolUse;
                        round.content.emplace_back(ai::ToolCallContent{
                                .id = "call_ext",
                                .name = tool_name,
                                .arguments = support::JsonValue::object_t{},
                                .raw_arguments = "{}",
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

/// Assemble one in-memory session whose only tool contributions are the given
/// extension sources. Every caller holds a clean HOME so settings isolation
/// matches the rest of the coding-agent shard.
[[nodiscard]] std::unique_ptr<coding_agent::AgentSession> make_extension_session(tests::RuntimeFixture& runtime,
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

TEST_CASE("the loader collects the tools of every extension source into its registry",
        "[coding_agent][extension][issue867][spec]") {
    StubExtensionToolSource first{"alpha", nullptr};
    StubExtensionToolSource second{"beta", nullptr};
    std::array<coding_agent::extensions::ExtensionToolSource*, 2> sources{&first, &second};

    coding_agent::extensions::ExtensionToolRegistry registry;
    REQUIRE(coding_agent::extensions::load_extension_tools(registry, sources).has_value());

    CHECK(registry.size() == 2);
    REQUIRE(registry.find("alpha") != nullptr);
    CHECK(registry.find("alpha")->definition.description == "Stub extension tool");
    REQUIRE(registry.find("beta") != nullptr);
    CHECK(registry.find("missing") == nullptr);
}

TEST_CASE("the registry rejects a duplicate extension tool name across sources",
        "[coding_agent][extension][issue867][spec]") {
    StubExtensionToolSource first{"dup", nullptr};
    StubExtensionToolSource second{"dup", nullptr};
    std::array<coding_agent::extensions::ExtensionToolSource*, 2> sources{&first, &second};

    coding_agent::extensions::ExtensionToolRegistry registry;
    auto loaded = coding_agent::extensions::load_extension_tools(registry, sources);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == support::ErrorCode::Validation);
    // The first registration survives; the duplicate never silently replaces it.
    CHECK(registry.size() == 1);
}

TEST_CASE("the runner registers an extension tool into the Agent registry with its policy intact",
        "[coding_agent][extension][issue867][spec]") {
    auto call_count = std::make_shared<std::size_t>(0);
    StubExtensionToolSource source{"stub_ext", call_count};
    std::array<coding_agent::extensions::ExtensionToolSource*, 1> sources{&source};
    coding_agent::extensions::ExtensionToolRegistry extension_tools;
    REQUIRE(coding_agent::extensions::load_extension_tools(extension_tools, sources).has_value());

    agent::ToolRegistry registry;
    REQUIRE(coding_agent::extensions::register_extension_tools(registry, std::move(extension_tools)).has_value());

    const auto* registered = registry.find("stub_ext");
    REQUIRE(registered != nullptr);
    CHECK(registered->definition.description == "Stub extension tool");
    // The concurrency policy and prompt snippet carry over unchanged: an
    // extension tool obeys the same Agent scheduling and prompt rules as a
    // built-in.
    CHECK(registered->concurrency == agent::ToolConcurrency::ParallelSafe);
    REQUIRE(registered->prompt_snippet.has_value());
    CHECK(*registered->prompt_snippet == "Run the stub extension tool");
    REQUIRE(static_cast<bool>(registered->execute));
}

TEST_CASE("the runner refuses an extension tool whose name collides with an existing Agent tool",
        "[coding_agent][extension][issue867][spec]") {
    agent::ToolRegistry registry;
    ai::Tool existing;
    existing.name = "read";
    existing.description = "the built-in reader";
    existing.parameters = support::JsonValue::object_t{{"type", "object"}};
    REQUIRE(registry.add(tests::make_fake_tool(std::move(existing),
                                 agent::ToolConcurrency::Exclusive,
                                 [](agent::ToolInvocation, std::stop_token, agent::ToolUpdateSink)
                                         -> boost::asio::awaitable<support::Expected<agent::AsyncToolExecutionResult>> {
                                     co_return agent::AsyncToolExecutionResult{};
                                 }))
                    .has_value());

    StubExtensionToolSource source{"read", nullptr};
    std::array<coding_agent::extensions::ExtensionToolSource*, 1> sources{&source};
    coding_agent::extensions::ExtensionToolRegistry extension_tools;
    REQUIRE(coding_agent::extensions::load_extension_tools(extension_tools, sources).has_value());

    auto registered = coding_agent::extensions::register_extension_tools(registry, std::move(extension_tools));
    REQUIRE_FALSE(registered.has_value());
    CHECK(registered.error().code == support::ErrorCode::Validation);
    // The built-in stays authoritative; no silent replacement happened.
    REQUIRE(registry.find("read") != nullptr);
    CHECK(registry.find("read")->definition.description == "the built-in reader");
}

TEST_CASE("an extension tool registered at assembly is discoverable and callable in the session",
        "[coding_agent][extension][issue867][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    auto call_count = std::make_shared<std::size_t>(0);
    std::vector<std::unique_ptr<coding_agent::extensions::ExtensionToolSource>> sources;
    sources.push_back(std::make_unique<StubExtensionToolSource>("stub_ext", call_count));
    auto session = make_extension_session(
            runtime, workspace, std::make_shared<ExtensionToolRoundProvider>("stub_ext"), std::move(sources));

    // Discoverable: the tool is on the Agent's active tool surface.
    CHECK(session_exposes_tool(*session, "stub_ext"));

    // Callable through the ordinary executor path: the model's tool call runs
    // the extension execute and its result lands as a tool result message.
    REQUIRE(tests::run_awaitable(runtime, session->prompt("use the stub tool")).has_value());
    CHECK(*call_count == 1);
    CHECK(tool_result_text(*session, "stub_ext") == std::optional<std::string>{"stub_ext: ok"});

    session->close();
}

TEST_CASE("an extension tool is visible to the session only when it is registered at assembly",
        "[coding_agent][extension][issue867][spec]") {
    tests::TempWorkspace workspace;
    const tests::EnvVarGuard home{"HOME", (workspace.path() / "agent").string()};
    tests::RuntimeFixture runtime;

    // The tool is loaded into a standalone extension registry: it exists and is
    // registered, but no session assembly has seen it.
    auto call_count = std::make_shared<std::size_t>(0);
    StubExtensionToolSource source{"stub_ext", call_count};
    std::array<coding_agent::extensions::ExtensionToolSource*, 1> sources{&source};
    coding_agent::extensions::ExtensionToolRegistry loaded_only;
    REQUIRE(coding_agent::extensions::load_extension_tools(loaded_only, sources).has_value());
    REQUIRE(loaded_only.find("stub_ext") != nullptr);

    // A session assembled without the source does not expose the tool: the
    // tool's existence is not what makes it visible.
    auto without =
            make_extension_session(runtime, workspace, std::make_shared<ExtensionToolRoundProvider>("stub_ext"), {});
    CHECK_FALSE(session_exposes_tool(*without, "stub_ext"));
    without->close();

    // A session assembled with the source exposes it. Together the two cases
    // separate registration-at-assembly (the property) from mere existence.
    std::vector<std::unique_ptr<coding_agent::extensions::ExtensionToolSource>> with_sources;
    with_sources.push_back(std::make_unique<StubExtensionToolSource>("stub_ext", call_count));
    auto with = make_extension_session(
            runtime, workspace, std::make_shared<ExtensionToolRoundProvider>("stub_ext"), std::move(with_sources));
    CHECK(session_exposes_tool(*with, "stub_ext"));
    with->close();
}

TEST_CASE("an extension tool renders through the existing fallback renderer, not a new seam",
        "[coding_agent][extension][issue867][spec]") {
    auto theme = tests::tool_render_theme();
    auto keybindings = tests::tool_render_keybindings();
    // The default registry registers renderers only for the built-in tools, so
    // an extension tool name must take the fallback pair exactly as pi's
    // extension tools do.
    coding_agent::tui::ToolExecutionComponent component(
            theme, keybindings, "stub_ext", "call_ext", R"({})", "/workspace");

    const auto screen = tests::render_tool_screen(component, 80);

    // The whole composed fallback block: the bold tool name, a blank row, and
    // the argument JSON. A named extension renderer would fill this in instead.
    CHECK(screen.visible == std::vector<std::string>{"", "stub_ext", "", "{}", ""});
}
