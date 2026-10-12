#include "coding_agent/cli/InteractiveTuiRun.hpp"
#include "coding_agent/tui/ExtensionUiHost.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"

#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "agent/harness/RuntimeRoot.hpp"
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <format>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using tests::drain_ready;

namespace {

using BootSessionResult = coding_agent::CreateAgentSessionResult;
using BootSessionCompletion = support::AsyncCompletion<BootSessionResult, support::Error>;

struct ListenerLifetime;

struct ListenerState {
    std::size_t registrations{0};
    std::vector<std::pair<std::size_t, std::string>> calls;
    bool consume_first_registration{false};
    bool rewrite_input{false};
    bool callbacks_destroyed_while_terminal_live{true};
    std::vector<std::string> second_listener_inputs;
    std::vector<std::weak_ptr<ListenerLifetime>> listener_lifetimes;
    std::vector<tui::CellPixelDimensions> cell_pixels_during_input;
};

struct ListenerLifetime {
    std::weak_ptr<ListenerState> state;
    tui::VirtualTerminal* terminal{nullptr}; // borrowed from the interactive fixture.

    ~ListenerLifetime() {
        if (auto owner = state.lock(); owner && terminal != nullptr) {
            owner->callbacks_destroyed_while_terminal_live &= terminal->modes().started;
        }
    }
};

[[nodiscard]] coding_agent::tui::ExtensionUiRegistrationFactory make_factory(
        const std::shared_ptr<ListenerState>& state, tui::VirtualTerminal* terminal = nullptr) {
    return [state, terminal](coding_agent::tui::ExtensionUiHost& host) {
        const auto registration = ++state->registrations;
        auto lifetime = std::make_shared<ListenerLifetime>();
        lifetime->state = state;
        lifetime->terminal = terminal;
        state->listener_lifetimes.emplace_back(lifetime);
        host.on_terminal_input([state, registration, terminal, lifetime](std::string_view input) {
            state->calls.emplace_back(registration, std::string{input});
            if (terminal != nullptr) {
                state->cell_pixels_during_input.push_back(terminal->capabilities().cell_pixels.value_or({}));
            }
            if (registration == 1 && state->consume_first_registration && input == "suppressed") {
                return tui::RawInputListenerResult::consume();
            }
            if (input == "\x03" || input == "\x04" || input.starts_with("/new")) {
                return tui::RawInputListenerResult::pass();
            }
            if (state->rewrite_input) {
                return tui::RawInputListenerResult::replace("rewritten");
            }
            return tui::RawInputListenerResult::pass();
        });
        host.on_terminal_input([state, registration, lifetime](std::string_view input) {
            state->second_listener_inputs.push_back(std::format("{}={}", registration, input));
            return tui::RawInputListenerResult::pass();
        });
    };
}

[[nodiscard]] support::AsyncResult<coding_agent::CreateAgentSessionResult> create_session_async(
        tests::RuntimeFixture& runtime, const tests::TempWorkspace& workspace) {
    tests::ModelsSessionOptions options;
    options.session_target = coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = tests::models_from_provider(tests::make_scripted_fake_provider());
    auto models = options.models;
    coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    return coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, tests::cli_fake_overrides(std::move(models)));
}

[[nodiscard]] std::string visible_screen(const tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

class InteractiveFixture final {
public:
    tests::TempWorkspace workspace;
    tests::TempWorkspace config;
    tests::RuntimeFixture runtime;
    std::optional<coding_agent::CreateAgentSessionResult> created;
    tui::VirtualTerminal terminal{{.columns = 100, .rows = 30}};
    boost::asio::io_context io;
    std::optional<support::ExpectedVoid> result;
    std::ostringstream error_stream;
    std::unique_ptr<tests::RuntimeLoopDriver> runtime_driver;
    std::optional<BootSessionCompletion> boot_completion;

    InteractiveFixture() {
        auto session = runtime.run(create_session_async(runtime, workspace));
        REQUIRE(session);
        created.emplace(std::move(*session));
        runtime_driver = std::make_unique<tests::RuntimeLoopDriver>(runtime);
    }

    void start(coding_agent::tui::ExtensionUiRegistrationFactory factory, bool use_cli_assembly = false) {
        coding_agent::tui::InteractiveSessionRun run = [&] {
            if (use_cli_assembly) {
                return cli::assemble_interactive_session_run(cli::InitialMessageResult{},
                        cli::CliConfig{},
                        /* runtime_root */ nullptr,
                        /* shared_runtime */ nullptr,
                        /* model_runtime_cli_fake */ false,
                        error_stream,
                        coding_agent::tui::BindExistingSession{created->session.get()},
                        {},
                        std::move(factory));
            }
            return coding_agent::tui::InteractiveSessionRunBuilder{}
                    .with_session(*created->session)
                    .with_agent_config_directory(config.path())
                    .with_extension_ui_registration_factory(std::move(factory))
                    .build();
        }();
        boost::asio::co_spawn(io,
                coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
                [this](std::exception_ptr exception, support::ExpectedVoid outcome) {
                    CHECK(exception == nullptr);
                    result.emplace(std::move(outcome));
                });
        drain_ready(io);
    }

    void start_deferred(coding_agent::tui::ExtensionUiRegistrationFactory factory) {
        auto replacement_sink =
                [this](std::size_t, coding_agent::runtime::AgentSessionCreationRequest, std::stop_token) {
                    return support::AsyncResult<BootSessionResult, support::Error>{
                            support::AsyncProducer<BootSessionResult, support::Error>{
                                    [this](BootSessionCompletion completion) noexcept {
                                        boot_completion.emplace(std::move(completion));
                                    }}};
                };
        auto runtime_io = std::shared_ptr<boost::asio::io_context>(&io, [](boost::asio::io_context*) {});
        auto runtime_root = std::make_shared<harness::RuntimeRoot>(std::move(runtime_io), harness::RuntimeLimits{});
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.workspace = workspace.path();
        request.session_target = coding_agent::InMemorySessionTarget{};
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                           .with_defer_boot(std::move(request))
                           .with_runtime_root(std::move(runtime_root))
                           .with_agent_config_directory(config.path())
                           .with_extension_ui_registration_factory(std::move(factory))
                           .with_async_session_replacement_sink(std::move(replacement_sink))
                           .build();
        boost::asio::co_spawn(io,
                coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
                [this](std::exception_ptr exception, support::ExpectedVoid outcome) {
                    CHECK(exception == nullptr);
                    result.emplace(std::move(outcome));
                });
        drain_ready(io);
    }

    void start_with_replacement(coding_agent::tui::ExtensionUiRegistrationFactory factory) {
        auto replacement_sink = [this](std::size_t,
                                        coding_agent::runtime::AgentSessionCreationRequest request,
                                        std::stop_token stop_token) {
            request.execution_runtime_target = runtime.make_target();
            request.session_facts.no_skills = true;
            request.session_facts.no_prompt_templates = true;
            auto models = tests::models_from_provider(tests::make_scripted_fake_provider());
            return coding_agent::create_agent_session_async(
                    std::move(request), std::nullopt, tests::cli_fake_overrides(std::move(models)), stop_token);
        };
        auto run = coding_agent::tui::InteractiveSessionRunBuilder{}
                           .with_session(*created->session)
                           .with_agent_config_directory(config.path())
                           .with_extension_ui_registration_factory(std::move(factory))
                           .with_async_session_replacement_sink(std::move(replacement_sink))
                           .build();
        boost::asio::co_spawn(io,
                coding_agent::tui::run_interactive_mode(terminal, std::move(run)),
                [this](std::exception_ptr exception, support::ExpectedVoid outcome) {
                    CHECK(exception == nullptr);
                    result.emplace(std::move(outcome));
                });
        drain_ready(io);
    }

    void type(std::string bytes) {
        REQUIRE(terminal.inject_input(std::move(bytes)));
        drain_ready(io);
    }

    void close() {
        REQUIRE(terminal.inject_input("\x03"));
        drain_ready(io);
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(tests::pump_until(io, [this] { return result.has_value(); }));
        REQUIRE(result);
        CHECK(*result);
    }

    void replace_session() {
        type("/new\r");
        REQUIRE(tests::pump_until(
                io, [this] { return visible_screen(terminal).find("New session started") != std::string::npos; }));
        drain_ready(io);
    }
};

} // namespace

TEST_CASE("CLI assembled extension registration rewrites input before the focused editor",
        "[coding_agent][tui][extension_ui][issue1013][spec]") {
    InteractiveFixture fixture;
    auto state = std::make_shared<ListenerState>();
    state->rewrite_input = true;
    fixture.start(make_factory(state), true);

    fixture.type("original");

    CHECK(state->registrations == 1);
    CHECK(visible_screen(fixture.terminal).find("rewritten") != std::string::npos);
    CHECK(visible_screen(fixture.terminal).find("original") == std::string::npos);
    fixture.close();
}

TEST_CASE("extension input consumption suppresses later listeners and editor insertion",
        "[coding_agent][tui][extension_ui][issue1013][spec]") {
    InteractiveFixture fixture;
    auto state = std::make_shared<ListenerState>();
    state->consume_first_registration = true;
    fixture.start(make_factory(state));

    fixture.type("suppressed");

    REQUIRE(state->calls.size() == 1);
    CHECK(state->calls.front().second == "suppressed");
    CHECK(state->second_listener_inputs.empty());
    CHECK(visible_screen(fixture.terminal).find("suppressed") == std::string::npos);
    fixture.close();
}

TEST_CASE("session replacement disposes old callbacks and orders fresh registrations without stale listeners",
        "[coding_agent][tui][extension_ui][issue1013][spec]") {
    InteractiveFixture fixture;
    auto state = std::make_shared<ListenerState>();
    state->rewrite_input = true;
    fixture.start_with_replacement(make_factory(state));

    fixture.replace_session();
    const auto old_registration_calls =
            std::count_if(state->calls.begin(), state->calls.end(), [](const auto& call) { return call.first == 1; });
    fixture.type("after");

    CHECK(state->registrations == 2);
    CHECK(std::count_if(state->calls.begin(), state->calls.end(), [](const auto& call) { return call.first == 1; }) ==
            old_registration_calls);
    const auto expected_call = std::pair<std::size_t, std::string>{2, "after"};
    CHECK(state->calls.back() == expected_call);
    CHECK(state->second_listener_inputs.back() == "2=rewritten");
    CHECK(visible_screen(fixture.terminal).find("rewritten") != std::string::npos);
    fixture.close();
}

TEST_CASE("session replacement keeps the same terminal interactive for its fresh registration",
        "[coding_agent][tui][extension_ui][issue1013][spec]") {
    InteractiveFixture fixture;
    auto state = std::make_shared<ListenerState>();
    state->rewrite_input = true;
    fixture.start_with_replacement(make_factory(state));

    fixture.replace_session();
    fixture.type("still-live");

    CHECK(fixture.terminal.modes().started);
    CHECK(state->registrations == 2);
    CHECK_FALSE(fixture.result.has_value());
    CHECK(visible_screen(fixture.terminal).find("rewritten") != std::string::npos);
    fixture.close();
}

TEST_CASE("deferred boot refreshes the extension registration after its session binds",
        "[coding_agent][tui][extension_ui][issue1013][spec]") {
    InteractiveFixture fixture;
    auto state = std::make_shared<ListenerState>();
    state->rewrite_input = true;
    fixture.start_deferred(make_factory(state));

    CHECK(state->registrations == 1);
    REQUIRE(fixture.boot_completion.has_value());
    REQUIRE(fixture.created.has_value());
    auto created = std::move(*fixture.created);
    fixture.created.reset();
    std::move (*fixture.boot_completion)(std::expected<BootSessionResult, support::Error>{std::move(created)});
    fixture.boot_completion.reset();
    REQUIRE(tests::pump_until(fixture.io, [&] { return state->registrations == 2; }));
    fixture.type("booted");

    CHECK(state->registrations == 2);
    REQUIRE(state->listener_lifetimes.size() == 2);
    CHECK(state->listener_lifetimes[0].expired());
    CHECK_FALSE(state->listener_lifetimes[1].expired());
    CHECK((state->calls.back() == std::pair<std::size_t, std::string>{2, "booted"}));
    CHECK(visible_screen(fixture.terminal).find("rewritten") != std::string::npos);
    fixture.close();
}

TEST_CASE("interactive close destroys extension listener callbacks before terminal shutdown",
        "[coding_agent][tui][extension_ui][issue1013][spec]") {
    InteractiveFixture fixture;
    auto state = std::make_shared<ListenerState>();
    fixture.start(make_factory(state, &fixture.terminal));
    REQUIRE(state->listener_lifetimes.size() == 1);
    CHECK_FALSE(state->listener_lifetimes.front().expired());

    fixture.close();

    CHECK(std::all_of(state->listener_lifetimes.begin(), state->listener_lifetimes.end(), [](const auto& lifetime) {
        return lifetime.expired();
    }));
    CHECK(state->callbacks_destroyed_while_terminal_live);
    CHECK_FALSE(fixture.terminal.modes().started);
}

TEST_CASE("extension listeners see cell-size replies after color replies are consumed by the toolkit",
        "[coding_agent][tui][extension_ui][issue1013][spec]") {
    InteractiveFixture fixture;
    auto state = std::make_shared<ListenerState>();
    fixture.start(make_factory(state, &fixture.terminal));

    fixture.type("\x1b]10;rgb:ffff/ffff/ffff\x07");
    CHECK(state->calls.empty());

    fixture.type("\x1b[6;20;10t");
    REQUIRE(state->calls.size() == 1);
    CHECK(state->calls.front().second == "\x1b[6;20;10t");
    CHECK((state->cell_pixels_during_input == std::vector<tui::CellPixelDimensions>{tui::CellPixelDimensions{}}));
    CHECK((fixture.terminal.capabilities().cell_pixels == tui::CellPixelDimensions{.width = 10, .height = 20}));
    CHECK(visible_screen(fixture.terminal).find("\x1b[6;20;10t") == std::string::npos);
    fixture.close();
}