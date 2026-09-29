// The Native TUI Pending Elicitation flow (issue #846, on top of #845's
// flow; spec #833 stories 26-27 and 28).
//
// This is the one case that crosses the whole presentation path: a real
// `AgentSession` over a scripted Upstream, the real `McpFlowController`, and
// a recording `ModalPresenter` — no virtual terminal, no live credentials, no
// network. The claim under test is the routing claim, which the dialog tests
// beside it cannot make: **the mode on the session's projection row decides
// which dialog the user is shown**, and a keypress in that dialog reaches the
// session's own answer API and out onto the wire as the retried request.
//
//   * a form-mode row opens the form dialog and nothing else;
//   * a URL-mode row opens the URL dialog and nothing else;
//   * typing into the form dialog and submitting it answers the suspended
//     call, so the dialog is the only thing between the user and the retry;
//   * a form the user cannot satisfy does not answer, and the dialog stays up;
//   * host Close withdraws the question rather than answering it.
//
// The dialogs themselves are exercised in `McpElicitationDialogTest.cpp` and
// `McpElicitationFormDialogTest.cpp`; the wire shape is proven in
// `tests/coding_agent/McpElicitationSessionTest.cpp`.

#include "coding_agent/tui/McpFlowController.hpp"

#include "ai/ModelStreamBridge.hpp"
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/McpElicitationDialog.hpp"
#include "coding_agent/tui/McpElicitationFormDialog.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/AgentRootFixture.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/ExpectedMacros.hpp"
#include "support/ModelsFixture.hpp"
#include "support/RenderedScreen.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedMcpTransport.hpp"
#include "support/StreamAdapterFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/support/Error.hpp>
#include "support/Json.hpp"
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Keys.hpp>
#include <cch/tui/Overlay.hpp>

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;
using coding_agent::McpElicitationAction;
using coding_agent::tui::McpElicitationDialog;
using coding_agent::tui::McpElicitationFormDialog;
using tests::ScriptedMcpAnswer;

namespace {

namespace runtime_ns = cch::coding_agent::runtime;

/// Every wait here is a cap, not a wait: a passing case returns as soon as
/// its condition holds.
constexpr std::chrono::milliseconds kBudget{5000};

/// Records every ModalPresenter call; the prompt slot holds the last
/// replacement component, which is how a case sees which dialog opened.
class RecordingModalPresenter final : public coding_agent::tui::ModalPresenter {
public:
    void show_overlay(std::unique_ptr<cch::tui::Overlay>) override { ++overlays_shown; }
    void close_overlay() override { ++overlays_closed; }
    void replace_prompt_slot(std::shared_ptr<cch::tui::Component> component) override {
        slot = std::move(component);
        ++slot_replacements;
    }
    void restore_prompt_slot() override {
        slot.reset();
        ++slot_restores;
    }
    void show_status(std::string) override {}
    void show_error(std::string) override {}
    void request_render() override { ++render_requests; }
    void invalidate() override { ++invalidations; }

    std::shared_ptr<cch::tui::Component> slot;
    int overlays_shown{0};
    int overlays_closed{0};
    int slot_replacements{0};
    int slot_restores{0};
    int render_requests{0};
    int invalidations{0};
};

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

/// One `tools/list` tool entry the model may call.
[[nodiscard]] support::JsonValue approvable_tool() {
    using JsonValue = support::JsonValue;
    return JsonValue::object_t{
            {"name", JsonValue("approve")},
            {"description", JsonValue("start an approval flow")},
            {"inputSchema", JsonValue::object_t{{"type", JsonValue("object")}, {"properties", JsonValue::object_t{}}}},
    };
}

/// A suspension in the named mode. The schema is the one the form dialog
/// renders, and the continuation token is the one the host echoes verbatim.
[[nodiscard]] support::JsonValue suspension(std::string_view mode) {
    using JsonValue = support::JsonValue;
    JsonValue::object_t request{
            {"id", JsonValue("r1")},
            {"type", JsonValue(std::string(mode))},
            {"message", JsonValue("Answer before the tool runs")},
    };
    if (mode == "url") {
        request.emplace("url", JsonValue("https://executor.invalid/mcp/approve/abc"));
    } else {
        request.emplace("schema", JsonValue::object_t{
                                            {"type", JsonValue("object")},
                                            {"title", JsonValue("Target")},
                                            {"properties", JsonValue::object_t{
                                                                    {"target", JsonValue::object_t{
                                                                                   {"type", JsonValue("string")},
                                                                                   {"title", JsonValue("Where")},
                                                                                   {"minLength", JsonValue(2)}}}}},
                                            {"required", JsonValue::array_t{JsonValue("target")}},
                                    });
    }
    return JsonValue::object_t{
            {"resultType", JsonValue("input_required")},
            {"requestState", JsonValue::object_t{{"token", JsonValue("suspension-1")}}},
            {"inputRequests", JsonValue::array_t{std::move(request)}},
    };
}

constexpr std::string_view kEagerServerSettings =
        R"({"mcpServers": {"executor": {"url": "https://mcp.example/mcp", "activation": "eager"}}})";
constexpr std::string_view kTrustedStore = R"({"executor": true})";

/// One scripted chat client that serves queued assistant messages in order.
class ElicitationProvider final : public tests::ScriptedProvider {
public:
    ElicitationProvider() : ScriptedProvider("sdk-host") {}

    [[nodiscard]] ai::ModelStream stream(
            ai::Model model, ai::AiContext context, coding_agent::ModelRuntimeTestStreamOptions options) override {
        return ai::detail::make_model_stream(
                [this, model = std::move(model), context = std::move(context), options = std::move(options)](
                        ai::AssistantEventSink sink)
                        -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
                    requests.push_back(tests::RecordedProviderRequest{model, context, options});
                    ai::AssistantMessage response = ai::assistant_text_message("done");
                    if (!responses.empty()) {
                        response = std::move(responses.front());
                        responses.erase(responses.begin());
                    }
                    response = tests::stamped_response(std::move(response), model);
                    if (sink) {
                        CCH_TRY_VOID(sink(ai::AssistantStartEvent{response}));
                    }
                    co_return response;
                });
    }

    std::vector<ai::AssistantMessage> responses;
    std::vector<tests::RecordedProviderRequest> requests;
};

[[nodiscard]] ai::AssistantMessage tool_call_response(
        std::string call_id, std::string tool_name, std::string raw_arguments) {
    auto message = ai::assistant_text_message("");
    message.content.clear();
    message.content.emplace_back(
            ai::tool_call_content(std::move(call_id), std::move(tool_name), std::move(raw_arguments)));
    return message;
}

/// A session, a scripted Upstream, and a flow controller wired to a recording
/// presenter: the production chain, minus the terminal.
struct ElicitationFlowFixture {
    tests::TempWorkspace workspace;
    std::filesystem::path agent_dir;
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    tests::EnvVarGuard deepseek_guard{"DEEPSEEK_API_KEY"};
    tests::RuntimeFixture runtime;
    tests::RuntimeLoopDriver driver;
    std::shared_ptr<tests::ScriptedMcpTransport> transport{std::make_shared<tests::ScriptedMcpTransport>()};
    std::shared_ptr<ElicitationProvider> client{std::make_shared<ElicitationProvider>()};
    std::unique_ptr<coding_agent::AgentSession> owned_session;
    boost::asio::io_context io;
    RecordingModalPresenter presenter;
    /// The palette and the keybinding slot outlive every flow that borrows
    /// them, the way the host's theme controller outlives its own flows.
    coding_agent::tui::LiveTheme theme{coding_agent::tui::builtin_dark_theme(),
            tui::TerminalColorCapability::TrueColor};
    std::shared_ptr<coding_agent::tui::SharedKeybindings> keybindings{
            std::make_shared<coding_agent::tui::SharedKeybindings>(test_keybindings())};
    std::shared_ptr<int> host_token{std::make_shared<int>(0)};
    std::shared_ptr<coding_agent::tui::McpFlowController> flows;
    std::string mode{"form"};

    ElicitationFlowFixture() : driver(runtime) {
        home_guard.set(workspace.path().string());
        agent_dir = tests::agent_root_under_home(workspace.path());
        std::filesystem::create_directories(agent_dir);
        kimi_guard.unset();
        deepseek_guard.unset();
        write(agent_dir / "settings.json", kEagerServerSettings);
        write(agent_dir / "mcp-trust.json", kTrustedStore);
        transport->answer("server/discover", ScriptedMcpAnswer{.result = tests::discover_result()});
        transport->answer("tools/list", ScriptedMcpAnswer{.result = tests::tool_list_result({approvable_tool()})});
    }

    static void write(const std::filesystem::path& path, std::string_view content) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }

    /// Suspend the first call in `mode` and complete every round after it.
    void script_calls() {
        transport->answer_with("tools/call",
                [this, round = std::size_t{0}](const support::JsonValue&) mutable {
                    return round++ == 0
                                   ? ScriptedMcpAnswer{.result = suspension(mode)}
                                   : ScriptedMcpAnswer{
                                             .result = tests::tool_call_result(
                                                     support::JsonValue::array_t{support::JsonValue::object_t{
                                                             {"type", support::JsonValue("text")},
                                                             {"text", support::JsonValue("answered")}}})};
                });
    }

    void create_session() {
        runtime_ns::AgentSessionCreationRequest request;
        request.execution_runtime_target = runtime.make_target();
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.session_target = coding_agent::InMemorySessionTarget{};
        request.mcp_transport = transport;
        auto models = tests::models_from_provider(client);
        auto created = runtime.run(coding_agent::create_agent_session_async(
                std::move(request), std::nullopt, tests::cli_fake_overrides(std::move(models))));
        REQUIRE(created.has_value());
        owned_session = std::move(created->session);
    }

    void create_flow() {
        coding_agent::tui::McpFlowHostHooks hooks;
        hooks.post_on_executor = [this](std::move_only_function<void()> action) mutable {
            boost::asio::post(io, std::move(action));
        };
        auto* session_pointer = owned_session.get();
        hooks.current_session = [session_pointer]() -> coding_agent::AgentSession* { return session_pointer; };
        hooks.live_theme = [this]() -> const coding_agent::tui::LiveTheme& { return theme; };
        hooks.action_generation = [] { return std::size_t{1}; };
        hooks.open_browser = [this](std::size_t, std::string url) { opened_browser = std::move(url); };
        flows = std::make_shared<coding_agent::tui::McpFlowController>(
                io.get_executor(), presenter, std::weak_ptr<void>{host_token}, std::move(hooks), keybindings);
    }

    [[nodiscard]] coding_agent::AgentSession& session() { return *owned_session; }

    /// Ask the controller to show whatever the session is blocked on, and let
    /// the posted work run. This is the frame tick's own call.
    void show_pending() {
        flows->show_pending();
        tests::drain_ready(io);
    }

    [[nodiscard]] std::shared_ptr<McpElicitationFormDialog> form_dialog() const {
        return std::dynamic_pointer_cast<McpElicitationFormDialog>(presenter.slot);
    }

    [[nodiscard]] std::shared_ptr<McpElicitationDialog> url_dialog() const {
        return std::dynamic_pointer_cast<McpElicitationDialog>(presenter.slot);
    }

    /// Start the prompt that makes the model call the Upstream tool, and hand
    /// back the handle that settles it: the run cannot be awaited
    /// synchronously, because the answer about to be given is what releases
    /// it.
    [[nodiscard]] std::thread start_prompt() {
        client->responses.push_back(tool_call_response("call-1", "mcp__executor__approve", "{}"));
        client->responses.push_back(ai::assistant_text_message("done"));
        auto& target = session();
        return std::thread([&target]() {
            boost::asio::io_context local;
            boost::asio::co_spawn(
                    local,
                    [&target]() -> boost::asio::awaitable<void> {
                        static_cast<void>(co_await target.prompt("deploy please"));
                        co_return;
                    },
                    boost::asio::detached);
            local.run();
        });
    }

    template <typename Condition> [[nodiscard]] static bool wait_until(Condition condition) {
        const auto deadline = std::chrono::steady_clock::now() + kBudget;
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return condition();
    }

    /// The `tools/call` request bodies, in order.
    [[nodiscard]] std::vector<std::string> call_bodies() const {
        std::vector<std::string> bodies;
        for (std::size_t index = 0; index < transport->request_count(); ++index) {
            const auto method = transport->recorded_method(index);
            REQUIRE(method.has_value());
            if (*method == "tools/call") {
                bodies.push_back(transport->requests().at(index).body);
            }
        }
        return bodies;
    }

    std::string opened_browser{};
};

} // namespace

TEST_CASE("a form-mode question opens the form dialog and its answer reaches the wire",
        "[coding_agent][tui][mcp][issue846][issue845][spec]") {
    ElicitationFlowFixture fixture;
    fixture.script_calls();
    fixture.create_session();
    fixture.create_flow();
    REQUIRE(fixture.wait_until([&fixture] { return fixture.session().mcp_published_tools().size() == 1; }));

    auto running = fixture.start_prompt();
    REQUIRE(fixture.wait_until([&fixture] { return fixture.session().pending_mcp_elicitations().size() == 1; }));
    fixture.show_pending();

    // The mode on the projection row is the whole of the routing: a form
    // question opens the form dialog, and nothing else does.
    const auto dialog = fixture.form_dialog();
    REQUIRE(dialog != nullptr);
    CHECK(fixture.url_dialog() == nullptr);
    CHECK(fixture.flows->showing() == fixture.session().pending_mcp_elicitations().front().elicitation_id);
    // The dialog rendered the schema the Upstream sent, not a generic prompt.
    auto rendered = dialog->render(80);
    REQUIRE(rendered.has_value());
    std::string screen;
    for (const auto& line : rendered->lines) {
        screen += tests::strip_ansi(line);
        screen += '\n';
    }
    CHECK(screen.find("Where") != std::string::npos);
    CHECK(screen.find("required") != std::string::npos);

    // A value below the field's `minLength` does not answer: the dialog stays
    // up and nothing is sent in the user's name.
    for (const char character : std::string_view{"x"}) {
        static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = std::string(1, character)}));
    }
    static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = "enter"}));
    REQUIRE(fixture.presenter.slot != nullptr);
    CHECK(fixture.transport->request_count("tools/call") == 1);
    CHECK_FALSE(dialog->field_error(0).empty());

    // Fixing it and submitting is one keypress path from the user to the
    // retried request.
    static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = "e"}));
    static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = "enter"}));
    REQUIRE(fixture.wait_until([&fixture] { return fixture.transport->request_count("tools/call") == 2; }));
    running.join();

    const auto bodies = fixture.call_bodies();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[1].find(R"("action":"accept")") != std::string::npos);
    CHECK(bodies[1].find(R"("content":{"target":"xe"})") != std::string::npos);
    CHECK(bodies[1].find(R"("requestState":{"token":"suspension-1"})") != std::string::npos);
    // The slot is restored, so the editor the dialog replaced is back.
    CHECK(fixture.presenter.slot == nullptr);
    CHECK(fixture.session().pending_mcp_elicitations().empty());
    CHECK(fixture.session().is_open());
}

TEST_CASE("a URL-mode question opens the URL dialog", "[coding_agent][tui][mcp][issue845][spec]") {
    ElicitationFlowFixture fixture;
    fixture.mode = "url";
    fixture.script_calls();
    fixture.create_session();
    fixture.create_flow();
    REQUIRE(fixture.wait_until([&fixture] { return fixture.session().mcp_published_tools().size() == 1; }));

    auto running = fixture.start_prompt();
    REQUIRE(fixture.wait_until([&fixture] { return fixture.session().pending_mcp_elicitations().size() == 1; }));
    fixture.show_pending();

    // The same routing claim, the other mode: a URL question opens the URL
    // dialog and nothing else.
    const auto dialog = fixture.url_dialog();
    REQUIRE(dialog != nullptr);
    CHECK(fixture.form_dialog() == nullptr);
    CHECK(dialog->server_id() == "executor");

    static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = "enter"}));
    REQUIRE(fixture.wait_until([&fixture] { return fixture.transport->request_count("tools/call") == 2; }));
    running.join();
    const auto bodies = fixture.call_bodies();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[1].find(R"("action":"accept")") != std::string::npos);
    // A URL question is answered with no content: it asked for no data.
    CHECK(bodies[1].find("content") == std::string::npos);
    CHECK(fixture.session().is_open());
}

TEST_CASE("host Close withdraws a pending question instead of answering it",
        "[coding_agent][tui][mcp][issue846][issue845][spec]") {
    ElicitationFlowFixture fixture;
    fixture.script_calls();
    fixture.create_session();
    fixture.create_flow();
    REQUIRE(fixture.wait_until([&fixture] { return fixture.session().mcp_published_tools().size() == 1; }));

    auto running = fixture.start_prompt();
    REQUIRE(fixture.wait_until([&fixture] { return fixture.session().pending_mcp_elicitations().size() == 1; }));
    fixture.show_pending();
    auto dialog = fixture.form_dialog();
    REQUIRE(dialog != nullptr);
    static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = "p"}));

    // The host closes: the question is withdrawn, and a withdrawn dialog has
    // no answer to give.
    fixture.flows->close();
    CHECK_FALSE(dialog->live());
    CHECK(fixture.presenter.slot == nullptr);
    // A keypress on the withdrawn dialog is inert in both directions.
    static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = "enter"}));
    static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = "d", .alt = true}));
    CHECK(fixture.transport->request_count("tools/call") == 1);

    fixture.session().close();
    running.join();
    CHECK(fixture.session().pending_mcp_elicitations().empty());
    // Nothing was re-sent on the way out.
    CHECK(fixture.transport->request_count("tools/call") == 1);
}

TEST_CASE("a question the user never reaches is not shown twice",
        "[coding_agent][tui][mcp][issue846][issue845][spec]") {
    ElicitationFlowFixture fixture;
    fixture.script_calls();
    fixture.create_session();
    fixture.create_flow();
    REQUIRE(fixture.wait_until([&fixture] { return fixture.session().mcp_published_tools().size() == 1; }));

    auto running = fixture.start_prompt();
    // The frame ticker asks on every frame; one question is one dialog, or the
    // user would be asked the same thing twice in two windows.
    REQUIRE(fixture.wait_until([&fixture] { return fixture.session().pending_mcp_elicitations().size() == 1; }));
    fixture.show_pending();
    CHECK(fixture.presenter.slot_replacements == 1);
    fixture.show_pending();
    CHECK(fixture.presenter.slot_replacements == 1);
    const auto dialog = fixture.form_dialog();
    REQUIRE(dialog != nullptr);

    // Answering it through the dialog retires it and asks the next question
    // immediately — and there is no next question, so the controller opens
    // nothing on the following frame either.
    static_cast<void>(dialog->handle_input(cch::tui::KeyEvent{.key = "d", .alt = true}));
    REQUIRE(fixture.wait_until([&fixture] { return fixture.transport->request_count("tools/call") == 2; }));
    running.join();
    CHECK(fixture.presenter.slot_replacements == 1);
    CHECK(fixture.session().pending_mcp_elicitations().empty());
    fixture.show_pending();
    CHECK(fixture.presenter.slot_replacements == 1);
    CHECK(fixture.form_dialog() == nullptr);
    // The slot the dialog replaced is back, so the editor is not lost with it.
    CHECK(fixture.presenter.slot == nullptr);
    CHECK(fixture.presenter.slot_restores >= 1);
}
