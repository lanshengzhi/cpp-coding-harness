// The `/mcp` interactive flow (spec #882, ticket #884): pi's
// `registerCommand("mcp")` handler (`pickServer`, `loginCommand`, the usage
// warning), the manage-loop hosting (servers menu -> server menu, cancel
// navigation, re-presentation on manager change), the sign-in screen driving
// the manager's McpSignInPrompt (browser hook, pasted redirect URL, the
// verbatim outcome lines), the warning surfacing, and the non-TUI status
// fallback. Driven headlessly: a scripted manager (no process, socket, or
// browser), a recording ModalPresenter, and a runtime loop driver.

#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/McpManagerFlow.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>

#include "support/AsyncResultBridge.hpp"
#include "support/RuntimeFixture.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace cch;

namespace {

using coding_agent::mcp::McpConfigEntry;
using coding_agent::mcp::McpConfigLoad;
using coding_agent::mcp::McpExposure;
using coding_agent::mcp::McpHttpServerConfig;
using coding_agent::mcp::McpStdioServerConfig;
using coding_agent::runtime::McpConnectionFactory;
using coding_agent::runtime::McpLiveConnection;
using coding_agent::runtime::McpLiveTool;
using coding_agent::runtime::McpManagerDependencies;
using coding_agent::runtime::McpRegisteredTool;
using coding_agent::runtime::McpServerState;
using coding_agent::runtime::McpSessionManager;
using coding_agent::runtime::McpSignInDriver;
using coding_agent::runtime::McpSignInPrompt;
using coding_agent::runtime::McpSurfaceTool;
using coding_agent::runtime::McpToolSurface;

// ── Headless presentation recorder ──────────────────────────────────────────

/// Records every ModalPresenter call; the prompt slot holds the last
/// replacement component for rendering assertions.
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
    void show_status(std::string text) override { statuses.push_back(std::move(text)); }
    void show_error(std::string text) override { errors.push_back(std::move(text)); }
    void show_warning(std::string text) override { warnings.push_back(std::move(text)); }
    void request_render() override { ++render_requests; }
    void invalidate() override { ++invalidations; }

    [[nodiscard]] bool status_contains(std::string_view needle) const {
        return std::ranges::any_of(
                statuses, [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    }

    std::shared_ptr<cch::tui::Component> slot;
    int overlays_shown{0};
    int overlays_closed{0};
    int slot_replacements{0};
    int slot_restores{0};
    int render_requests{0};
    int invalidations{0};
    std::vector<std::string> statuses;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// ── Scripted manager seams ──────────────────────────────────────────────────

class TestConnection final : public McpLiveConnection {
public:
    std::string name;
    McpServerState state_value{McpServerState::Connected};
    std::vector<McpLiveTool> offered_tools;
    bool resources{false};
    std::size_t resource_count_value{0};
    std::optional<std::string> failure;
    std::optional<std::string> stderr;
    bool oauth{false};
    std::string oauth_url_value{"https://mcp.example.com/mcp"};
    std::optional<std::string> instructions_value;
    int reconnect_calls{0};
    int sign_out_calls{0};
    McpLiveConnection::ToolsChangedListener tools_changed;
    McpLiveConnection::ResourcesChangedListener resources_changed;

    [[nodiscard]] const std::string& server_name() const noexcept override { return name; }
    [[nodiscard]] McpServerState state() const noexcept override { return state_value; }
    [[nodiscard]] const std::vector<McpLiveTool>& tools() const noexcept override { return offered_tools; }
    [[nodiscard]] bool has_resources() const noexcept override { return resources; }
    [[nodiscard]] std::size_t resource_count() const noexcept override { return resource_count_value; }
    [[nodiscard]] const std::optional<std::string>& error() const noexcept override { return failure; }
    [[nodiscard]] const std::optional<std::string>& stderr_tail() const noexcept override { return stderr; }
    [[nodiscard]] bool uses_oauth() const noexcept override { return oauth; }
    [[nodiscard]] const std::string& oauth_url() const noexcept override { return oauth_url_value; }
    [[nodiscard]] const std::optional<std::string>& instructions() const noexcept override {
        return instructions_value;
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> call_tool(std::string_view,
            support::JsonValue,
            std::stop_token,
            coding_agent::mcp::McpServerConnection::ProgressCallback = {}) override {
        return support::AsyncResult<support::JsonValue>{
                support::JsonValue{support::JsonValue::object_t{{"content", support::JsonValue::array_t{}}}}};
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> resources_page(
            std::optional<std::string>, std::stop_token) override {
        return support::AsyncResult<support::JsonValue>{
                support::JsonValue{support::JsonValue::object_t{{"resources", support::JsonValue::array_t{}}}}};
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> resource_templates_page(
            std::optional<std::string>, std::stop_token) override {
        return support::AsyncResult<support::JsonValue>{
                support::JsonValue{support::JsonValue::object_t{{"resourceTemplates", support::JsonValue::array_t{}}}}};
    }
    [[nodiscard]] support::AsyncResult<support::JsonValue> read_resource(std::string, std::stop_token) override {
        return support::AsyncResult<support::JsonValue>{
                support::JsonValue{support::JsonValue::object_t{{"contents", support::JsonValue::array_t{}}}}};
    }
    [[nodiscard]] support::AsyncResult<void> reconnect() override {
        ++reconnect_calls;
        state_value = McpServerState::Connected;
        failure.reset();
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    [[nodiscard]] support::AsyncResult<void> sign_out() override {
        ++sign_out_calls;
        state_value = McpServerState::NeedsAuth;
        return support::AsyncResult<void>{support::ExpectedVoid{}};
    }
    void close() noexcept override {}
    void set_tools_changed_listener(ToolsChangedListener listener) override { tools_changed = std::move(listener); }
    void set_resources_changed_listener(ResourcesChangedListener listener) override {
        resources_changed = std::move(listener);
    }

    void fire_tools_changed() {
        if (tools_changed) {
            tools_changed();
        }
    }
};

class TestFactory final : public McpConnectionFactory {
public:
    std::map<std::string, std::shared_ptr<TestConnection>> outcomes;
    [[nodiscard]] support::AsyncResult<std::shared_ptr<McpLiveConnection>> connect(
            const McpConfigEntry& entry) override {
        const auto found = outcomes.find(entry.name);
        if (found == outcomes.end()) {
            return support::AsyncResult<std::shared_ptr<McpLiveConnection>>{
                    std::unexpected(support::make_error(support::ErrorCode::Process, "no connection"))};
        }
        return support::AsyncResult<std::shared_ptr<McpLiveConnection>>{
                std::shared_ptr<McpLiveConnection>{found->second}};
    }
};

class TestSurface final : public McpToolSurface {
public:
    /// The production surface always has the codemode extension's
    /// registration, so the discovery check finds it and the reachability
    /// warning stays silent for codemode-only configs.
    bool codemode_registered{true};

    void register_tool(McpRegisteredTool) override {}
    void register_resource_tools(
            McpExposure, std::vector<std::shared_ptr<coding_agent::mcp::McpResourceServer>>) override {}
    void set_active_tools(std::vector<std::string>) override {}
    [[nodiscard]] std::vector<std::string> active_tools() const override { return {}; }
    [[nodiscard]] std::vector<McpSurfaceTool> all_tools() const override {
        if (!codemode_registered) {
            return {};
        }
        return {McpSurfaceTool{"codemode", McpExposure::Codemode}};
    }
};

/// The scripted sign-in driver: plays the session-level prompt like the
/// production driver (shows the authorization URL, awaits the pasted redirect
/// URL) and reports the scripted failure message (nullopt = success).
class TestAuth final : public McpSignInDriver {
public:
    std::optional<std::string> sign_in_result{std::nullopt};
    bool credentials_removed{false};
    int sign_in_calls{0};
    int sign_out_calls{0};

    [[nodiscard]] support::AsyncResult<std::optional<std::string>> sign_in(
            const McpConfigEntry&, const McpSignInPrompt& prompt) override {
        ++sign_in_calls;
        const auto result = sign_in_result;
        return support::detail::make_async_result(
                [prompt, result]() mutable -> boost::asio::awaitable<support::Expected<std::optional<std::string>>> {
                    if (prompt.show_authorization_url) {
                        prompt.show_authorization_url("https://auth.example.com/authorize?client=scripted");
                    }
                    if (prompt.prompt_for_redirect_url) {
                        // Await the paste (nullopt when the race or the
                        // screen cancels); the scripted outcome decides.
                        static_cast<void>(co_await prompt.prompt_for_redirect_url());
                    }
                    // The pasted URL reached the flow: report the scripted
                    // terminal outcome.
                    co_return result;
                });
    }
    [[nodiscard]] support::AsyncResult<bool> sign_out(const McpConfigEntry&) override {
        ++sign_out_calls;
        return support::AsyncResult<bool>{credentials_removed};
    }
};

// ── View helpers (the landed panel test's rendering pattern) ────────────────

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    const std::array<std::string_view, 1> copy_action{"app.message.copy"};
    auto application = coding_agent::tui::app_keybinding_definitions(copy_action);
    REQUIRE(application);
    request.definitions.insert(request.definitions.end(),
            std::make_move_iterator(application->begin()),
            std::make_move_iterator(application->end()));
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] std::shared_ptr<coding_agent::tui::SharedKeybindings> test_shared_keybindings() {
    return std::make_shared<coding_agent::tui::SharedKeybindings>(test_keybindings());
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) {
                ++index;
            }
            if (index < text.size()) {
                ++index;
            }
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::vector<std::string> screen_of(cch::tui::Component& component, std::size_t width = 80) {
    auto rendered = component.render(width);
    REQUIRE(rendered);
    std::vector<std::string> lines;
    lines.reserve(rendered->lines.size());
    for (const auto& line : rendered->lines) {
        auto plain = strip_ansi(line);
        const auto last = plain.find_last_not_of(' ');
        lines.push_back(last == std::string::npos ? std::string{} : plain.substr(0, last + 1));
    }
    return lines;
}

[[nodiscard]] bool screen_contains(const std::vector<std::string>& screen, std::string_view needle) {
    return std::ranges::any_of(screen, [&](const std::string& line) { return line.find(needle) != std::string::npos; });
}

void press(cch::tui::InputHandler& handler, cch::tui::KeyEvent key) {
    static_cast<void>(handler.handle_input(cch::tui::InputEventVariant{std::move(key)}));
}

void type(cch::tui::InputHandler& handler, std::string_view text) {
    for (const char character : text) {
        press(handler, cch::tui::KeyEvent{.key = std::string(1, character)});
    }
}

// ── The flow fixture ────────────────────────────────────────────────────────

[[nodiscard]] McpConfigEntry stdio_entry(std::string name, bool enabled = true) {
    McpStdioServerConfig config;
    config.name = name;
    config.command = "python3";
    McpConfigEntry entry;
    entry.name = std::move(name);
    entry.enabled = enabled;
    entry.source = "/home/u/.pi/agent/mcp.json";
    entry.config = std::move(config);
    return entry;
}

[[nodiscard]] McpConfigEntry http_entry(std::string name) {
    McpHttpServerConfig config;
    config.name = name;
    config.url = "https://mcp.example.com/mcp";
    config.oauth = coding_agent::mcp::McpOAuthConfig{};
    McpConfigEntry entry;
    entry.name = std::move(name);
    entry.enabled = true;
    entry.source = "/home/u/.pi/agent/mcp.json";
    entry.config = std::move(config);
    return entry;
}

[[nodiscard]] McpConfigLoad config_with(std::vector<McpConfigEntry> servers) {
    // Named-member construction rather than a designated initializer: the
    // warning gate recompiles with -Werror=missing-field-initializers, which
    // rejects an aggregate that omits the trailing `errors`/`project_config`
    // members.
    McpConfigLoad config;
    config.servers = std::move(servers);
    return config;
}

struct FlowFixture {
    /// The flow's private serialized domain: posts and spawned flows run
    /// here, pumped by `pump` (RuntimeFixture::loop is private to its
    /// driver; the flow needs no Runtime workers, only a serialized loop).
    boost::asio::io_context flow_io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{
            boost::asio::make_work_guard(flow_io)};
    std::unique_ptr<std::thread> pump;
    std::shared_ptr<TestFactory> factory = std::make_shared<TestFactory>();
    std::shared_ptr<TestSurface> surface = std::make_shared<TestSurface>();
    std::shared_ptr<TestAuth> auth = std::make_shared<TestAuth>();
    std::unique_ptr<McpSessionManager> manager;
    std::shared_ptr<coding_agent::tui::McpManagerFlow> flow;
    RecordingModalPresenter presenter;
    std::shared_ptr<void> host_lifetime = std::make_shared<int>(0);
    tests::RuntimeFixture runtime;
    std::vector<std::string> opened_urls;
    std::vector<std::string> copied;

    FlowFixture() = default;
    FlowFixture(FlowFixture&&) = delete;
    FlowFixture& operator=(FlowFixture&&) = delete;
    ~FlowFixture() {
        work.reset();
        flow_io.stop();
        if (pump && pump->joinable()) {
            pump->join();
        }
    }

    [[nodiscard]] static std::unique_ptr<FlowFixture> make(McpConfigLoad config,
            std::vector<std::pair<std::string, std::shared_ptr<TestConnection>>> connections,
            bool auto_enable_codemode = true) {
        auto fixture = std::make_unique<FlowFixture>();
        config.auto_enable_codemode = auto_enable_codemode;
        McpManagerDependencies dependencies;
        dependencies.connections = fixture->factory;
        dependencies.tools = fixture->surface;
        dependencies.auth = fixture->auth;
        fixture->manager = std::make_unique<McpSessionManager>(
                std::move(config), "/home/u/.pi/agent", std::vector<std::string>{}, std::move(dependencies));
        for (auto& [name, connection] : connections) {
            connection->name = name;
            fixture->factory->outcomes[name] = std::move(connection);
        }
        REQUIRE(fixture->runtime.run(fixture->manager->start()).has_value());
        fixture->pump = std::make_unique<std::thread>([&io = fixture->flow_io] { io.run(); });

        coding_agent::tui::McpFlowHostHooks hooks;
        hooks.post_on_executor = [&io = fixture->flow_io](std::move_only_function<void()> action) {
            boost::asio::post(io, std::move(action));
        };
        // Keep the `start` closure alive until the spawned coroutine reaches
        // its terminal completion: the coroutine frame may reference its
        // closure (the engine's spawn_flow documents the same ADR 0040
        // mechanism).
        hooks.spawn_flow = [&io = fixture->flow_io](
                                   std::move_only_function<boost::asio::awaitable<void>()> start, std::string) {
            auto owner = std::make_shared<std::move_only_function<boost::asio::awaitable<void>()>>(std::move(start));
            boost::asio::co_spawn(
                    io,
                    [owner]() mutable -> boost::asio::awaitable<void> { co_await (*owner)(); },
                    boost::asio::detached);
        };
        hooks.mcp_manager = [fixture = fixture.get()]() -> McpSessionManager* { return fixture->manager.get(); };
        hooks.live_theme = []() -> const coding_agent::tui::LiveTheme& {
            static const auto theme = test_theme();
            return theme;
        };
        hooks.is_live = [] { return true; };
        hooks.overlay_active = [] { return false; };
        hooks.open_browser = [fixture = fixture.get()](
                                     std::string url) { fixture->opened_urls.push_back(std::move(url)); };
        hooks.copy_text = [fixture = fixture.get()](std::string text) { fixture->copied.push_back(std::move(text)); };
        fixture->flow = std::make_shared<coding_agent::tui::McpManagerFlow>(fixture->flow_io.get_executor(),
                fixture->presenter,
                fixture->host_lifetime,
                std::move(hooks),
                test_shared_keybindings());
        fixture->flow->refresh_completion();
        return fixture;
    }

    [[nodiscard]] coding_agent::tui::McpManagerView* view() {
        return dynamic_cast<coding_agent::tui::McpManagerView*>(presenter.slot.get());
    }

    /// Run the loop until the condition holds (the loop driver pumps on its
    /// own thread; this waits on the test thread).
    template <typename Predicate>
    [[nodiscard]] bool wait_until(Predicate condition, std::chrono::milliseconds budget = std::chrono::seconds{5}) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return condition();
    }
};

} // namespace

TEST_CASE("/mcp opens the servers menu panel and escape closes it", "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    auto fixture = FlowFixture::make(config_with({stdio_entry("echo")}), {{"echo", connection}});

    fixture->flow->handle_command("");
    REQUIRE(fixture->wait_until([&] { return fixture->presenter.slot_replacements == 1; }));
    auto* panel = fixture->view();
    REQUIRE(panel != nullptr);
    const auto screen = screen_of(*panel);
    CHECK(screen_contains(screen, "MCP servers"));
    CHECK(screen_contains(screen, "echo"));
    CHECK(screen_contains(screen, "connected"));

    press(*panel, cch::tui::KeyEvent{.key = "escape"});
    REQUIRE(fixture->wait_until([&] { return fixture->presenter.slot_restores == 1; }));
    CHECK(fixture->view() == nullptr);
}

TEST_CASE("/mcp navigates into a server menu and runs pi's reconnect action",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    connection->offered_tools = {McpLiveTool{.name = "alpha", .description = "a"}};
    auto fixture = FlowFixture::make(config_with({stdio_entry("echo")}), {{"echo", connection}});

    fixture->flow->handle_command("");
    REQUIRE(fixture->wait_until([&] { return fixture->view() != nullptr; }));
    auto* panel = fixture->view();
    press(*panel, cch::tui::KeyEvent{.key = "enter"});
    REQUIRE(fixture->wait_until([&] { return screen_contains(screen_of(*panel), "MCP server echo"); }));

    // Tools is the first row for a connected server; move to Reconnect before confirming.
    press(*panel, cch::tui::KeyEvent{.key = "down"});
    press(*panel, cch::tui::KeyEvent{.key = "enter"});
    REQUIRE(fixture->wait_until([&] { return connection->reconnect_calls == 1; }));
    CHECK(screen_contains(screen_of(*panel), "MCP server echo"));

    // Back out to the servers menu, then close.
    press(*panel, cch::tui::KeyEvent{.key = "escape"});
    REQUIRE(fixture->wait_until([&] { return screen_contains(screen_of(*panel), "MCP servers"); }));
    press(*panel, cch::tui::KeyEvent{.key = "escape"});
    REQUIRE(fixture->wait_until([&] { return fixture->presenter.slot_restores == 1; }));
}

TEST_CASE("the open panel re-presents on a manager change", "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    auto fixture = FlowFixture::make(config_with({stdio_entry("echo")}), {{"echo", connection}});

    fixture->flow->handle_command("");
    REQUIRE(fixture->wait_until([&] { return fixture->view() != nullptr; }));
    auto* panel = fixture->view();
    CHECK(screen_contains(screen_of(*panel), "connected"));

    // A server-side change (a dropped connection reporting failure) reaches
    // the open menu through the manager's change listener.
    connection->state_value = McpServerState::Failed;
    connection->failure = "boom";
    connection->fire_tools_changed();
    REQUIRE(fixture->wait_until([&] { return screen_contains(screen_of(*panel), "failed"); }));
}

TEST_CASE("/mcp reconnect reports pi's line and pi's pick errors", "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    connection->offered_tools = {
            McpLiveTool{.name = "alpha", .description = "a"},
            McpLiveTool{.name = "beta", .description = "b"},
    };
    auto fixture = FlowFixture::make(config_with({stdio_entry("echo")}), {{"echo", connection}});
    fixture->flow->run_reconnect("echo");
    REQUIRE(fixture->wait_until([&] { return fixture->presenter.status_contains("Reconnected to MCP server"); }));
    CHECK(connection->reconnect_calls == 1);
    CHECK(fixture->presenter.status_contains("Reconnected to MCP server \"echo\" (connected · 2 tools)."));
    CHECK(fixture->presenter.errors.empty());
}

TEST_CASE("/mcp reconnect with no name picks the single enabled server", "[coding_agent][tui][mcp][issue884][spec]") {
    auto first = std::make_shared<TestConnection>();
    auto second = std::make_shared<TestConnection>();
    McpConfigLoad config;
    config.servers = {stdio_entry("echo"), stdio_entry("off", /* enabled */ false)};
    // Two configured entries but only one enabled (the disabled one never
    // connects), so pi's pick has exactly one candidate.
    auto fixture = FlowFixture::make(std::move(config), {{"echo", first}, {"off", second}});
    CHECK(fixture->factory->outcomes.at("off") != nullptr);

    fixture->flow->run_reconnect("");
    REQUIRE(fixture->wait_until([&] { return first->reconnect_calls == 1; }));
    CHECK(fixture->presenter.status_contains("Reconnected to MCP server \"echo\""));
}

TEST_CASE("/mcp reconnect of a named server without a connection reports pi's notice",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto fixture = FlowFixture::make(config_with({stdio_entry("off", /* enabled */ false)}), {});
    fixture->flow->run_reconnect("off");
    REQUIRE(fixture->wait_until([&] { return !fixture->presenter.errors.empty(); }));
    CHECK(fixture->presenter.errors.back() == "No enabled MCP server to reconnect.");
}

TEST_CASE("/mcp login signs in through the redirect screen with pi's outcome lines",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    connection->state_value = McpServerState::NeedsAuth;
    connection->oauth = true;
    connection->offered_tools = {McpLiveTool{.name = "alpha", .description = "a"}};
    auto fixture = FlowFixture::make(config_with({http_entry("remote")}), {{"remote", connection}});

    fixture->flow->run_login("");
    // The panel opens with the contacting status, then the redirect screen
    // once the authorization URL arrives; the browser hook fired.
    REQUIRE(fixture->wait_until([&] { return fixture->view() != nullptr; }));
    REQUIRE(fixture->wait_until([&] {
        return fixture->view()->title() == "Sign in to remote" && !fixture->view()->authorization_url().empty();
    }));
    CHECK(fixture->opened_urls == std::vector<std::string>{"https://auth.example.com/authorize?client=scripted"});
    CHECK(screen_contains(screen_of(*fixture->view()), "Approve access in your browser"));

    // Paste the redirect URL the browser would have produced.
    type(*fixture->view(), "http://127.0.0.1:9/callback?code=xyz");
    press(*fixture->view(), cch::tui::KeyEvent{.key = "enter"});
    REQUIRE(fixture->wait_until([&] { return fixture->auth->sign_in_calls == 1; }));
    // Success: the manager reconnected, the panel closed (the command path),
    // and pi's line names the tool count the reconnected server offers.
    REQUIRE(fixture->wait_until([&] { return fixture->presenter.slot_restores == 1; }));
    CHECK(connection->reconnect_calls == 1);
    CHECK(fixture->presenter.status_contains("Signed in to MCP server \"remote\" (1 tools)."));
}

TEST_CASE("/mcp login of a non-OAuth server and the empty pick report pi's eligibility line",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    auto fixture = FlowFixture::make(config_with({stdio_entry("echo")}), {{"echo", connection}});

    fixture->flow->run_login("");
    REQUIRE(fixture->wait_until([&] {
        return fixture->presenter.status_contains("No enabled MCP server uses OAuth. Only HTTP servers without an "
                                                  "Authorization header do.");
    }));

    fixture->flow->run_login("echo");
    REQUIRE(fixture->wait_until([&] { return !fixture->presenter.errors.empty(); }));
    CHECK(fixture->presenter.errors.back() ==
            "No enabled MCP server uses OAuth. Only HTTP servers without an Authorization header do.");
    CHECK(fixture->auth->sign_in_calls == 0);
}

TEST_CASE("/mcp login with several needs-auth servers lists them with pi's run-login line",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto first = std::make_shared<TestConnection>();
    first->state_value = McpServerState::NeedsAuth;
    first->oauth = true;
    auto second = std::make_shared<TestConnection>();
    second->state_value = McpServerState::NeedsAuth;
    second->oauth = true;
    McpConfigLoad config;
    config.servers = {http_entry("alpha"), http_entry("beta")};
    auto fixture = FlowFixture::make(std::move(config), {{"alpha", first}, {"beta", second}});

    fixture->flow->run_login("");
    REQUIRE(fixture->wait_until([&] { return fixture->presenter.status_contains("run /mcp login alpha"); }));
    CHECK(fixture->presenter.status_contains("run /mcp login beta"));
    CHECK(fixture->auth->sign_in_calls == 0);
    CHECK(fixture->view() == nullptr);
}

TEST_CASE("/mcp logout reports pi's stored-credentials lines", "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    connection->oauth = true;
    auto fixture = FlowFixture::make(config_with({http_entry("remote")}), {{"remote", connection}});

    fixture->auth->credentials_removed = false;
    fixture->flow->run_logout("remote");
    REQUIRE(fixture->wait_until(
            [&] { return fixture->presenter.status_contains("No stored credentials for MCP server \"remote\"."); }));

    fixture->auth->credentials_removed = true;
    fixture->flow->run_logout("remote");
    REQUIRE(fixture->wait_until(
            [&] { return fixture->presenter.status_contains("Signed out of MCP server \"remote\"."); }));
    CHECK(fixture->auth->sign_out_calls == 2);
    CHECK(connection->sign_out_calls == 2);
}

TEST_CASE("/mcp with an unknown action or extra argument shows pi's usage warning",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    auto fixture = FlowFixture::make(config_with({stdio_entry("echo")}), {{"echo", connection}});

    fixture->flow->handle_command("frobnicate");
    REQUIRE(fixture->wait_until([&] { return !fixture->presenter.warnings.empty(); }));
    CHECK(fixture->presenter.warnings.back() ==
            "Usage: /mcp, /mcp login [server], /mcp logout [server], /mcp reconnect [server]");

    fixture->flow->handle_command("login one two");
    REQUIRE(fixture->wait_until([&] { return fixture->presenter.warnings.size() == 2; }));
}

TEST_CASE("the discovery warning reaches the notification channel and the snapshot follows changes",
        "[coding_agent][tui][mcp][issue884][spec]") {
    auto connection = std::make_shared<TestConnection>();
    connection->offered_tools = {McpLiveTool{.name = "alpha", .description = "a"}};
    // autoEnableCodemode:false: the manager latched the warning at start(),
    // before the flow subscribed.
    auto fixture = FlowFixture::make(
            config_with({stdio_entry("echo")}), {{"echo", connection}}, /* auto_enable_codemode */ false);

    REQUIRE(fixture->wait_until([&] { return !fixture->presenter.warnings.empty(); }));
    CHECK(fixture->presenter.warnings.front() ==
            "MCP tools are only reachable from the codemode or tool_search tool, but neither is active "
            "(autoEnableCodemode is false); they cannot be called.");

    // The completion snapshot reflects the live manager and refreshes on
    // change.
    REQUIRE(fixture->flow->completion_snapshot()->servers.size() == 1);
    connection->offered_tools.push_back(McpLiveTool{.name = "beta", .description = "b"});
    connection->fire_tools_changed();
    REQUIRE(fixture->wait_until([&] {
        const auto snapshot = fixture->flow->completion_snapshot();
        return !snapshot->servers.empty() && snapshot->servers.front().connection &&
               snapshot->servers.front().connection->tools.size() == 2;
    }));
}

TEST_CASE("/mcp without a manager prints pi's non-TUI status report", "[coding_agent][tui][mcp][issue884][spec]") {
    boost::asio::io_context flow_io;
    auto work = boost::asio::make_work_guard(flow_io);
    std::thread pump([&flow_io] { flow_io.run(); });
    const auto stop_pump = [&] {
        work.reset();
        flow_io.stop();
        if (pump.joinable()) {
            pump.join();
        }
    };
    RecordingModalPresenter presenter;
    std::shared_ptr<void> host_lifetime = std::make_shared<int>(0);
    coding_agent::tui::McpFlowHostHooks hooks;
    hooks.post_on_executor = [&flow_io](std::move_only_function<void()> action) {
        boost::asio::post(flow_io, std::move(action));
    };
    hooks.spawn_flow = [&flow_io](std::move_only_function<boost::asio::awaitable<void>()> start, std::string) {
        auto owner = std::make_shared<std::move_only_function<boost::asio::awaitable<void>()>>(std::move(start));
        boost::asio::co_spawn(
                flow_io,
                [owner]() mutable -> boost::asio::awaitable<void> { co_await (*owner)(); },
                boost::asio::detached);
    };
    hooks.mcp_manager = [] { return static_cast<McpSessionManager*>(nullptr); };
    hooks.live_theme = []() -> const coding_agent::tui::LiveTheme& {
        static const auto theme = test_theme();
        return theme;
    };
    hooks.is_live = [] { return true; };
    auto flow = std::make_shared<coding_agent::tui::McpManagerFlow>(
            flow_io.get_executor(), presenter, host_lifetime, std::move(hooks), test_shared_keybindings());

    flow->handle_command("");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (presenter.statuses.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(!presenter.statuses.empty());
    // pi's empty message names the global mcp.json.
    CHECK(presenter.statuses.front().find("No MCP servers configured") != std::string::npos);
    CHECK(presenter.slot == nullptr);
    stop_pump();
}
