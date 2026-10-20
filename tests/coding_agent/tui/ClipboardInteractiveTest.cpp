// #993 clipboard product action: the CLI clipboard assembly
// (`assemble_interactive_session_run`) driven on a virtual terminal — `/copy`
// through the shell helper order and the OSC 52 terminal route, paste through
// the registered Linux helper reader, and Close cancelling an in-flight helper.

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/cli/InteractiveTuiRun.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"

#include "FakeClipboardBin.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ImageFixture.hpp"
#include "support/ModelsFixture.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/ScriptedProvider.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/ai/Content.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/NativeClipboard.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using testing::FakeClipboardBin;

using Clock = std::chrono::steady_clock;

/// The terminal that owns the copy, exactly as the production engine installs
/// it (`terminal_.write`): every byte reaches the virtual terminal except the
/// OSC 52 clipboard sequence, which is recorded here. `VirtualTerminal` rejects
/// unknown OSC codes and pi's copy never inspects the write's result, so
/// recording the bytes is both the copy evidence and a write that cannot
/// disturb the screen assertions.
class Osc52RecordingTerminal final : public cch::tui::Terminal {
public:
    explicit Osc52RecordingTerminal(cch::tui::VirtualTerminal& terminal) : terminal_(terminal) {}

    [[nodiscard]] support::ExpectedVoid start(
            cch::tui::TerminalInputSink input_sink, cch::tui::TerminalResizeSink resize_sink) override {
        return terminal_.start(std::move(input_sink), std::move(resize_sink));
    }
    [[nodiscard]] support::ExpectedVoid stop() override { return terminal_.stop(); }
    [[nodiscard]] cch::tui::TerminalDimensions dimensions() const override { return terminal_.dimensions(); }
    [[nodiscard]] cch::tui::TerminalCapabilities capabilities() const override { return terminal_.capabilities(); }
    [[nodiscard]] cch::tui::TerminalModeState modes() const override { return terminal_.modes(); }
    [[nodiscard]] support::ExpectedVoid clear_screen() override { return terminal_.clear_screen(); }
    [[nodiscard]] support::ExpectedVoid write(std::string_view output) override {
        if (output.starts_with("\x1b]52;c;")) {
            sequences_.emplace_back(output);
            return {};
        }
        return terminal_.write(output);
    }
    [[nodiscard]] support::ExpectedVoid set_cursor(cch::tui::CursorPosition position) override {
        return terminal_.set_cursor(position);
    }
    [[nodiscard]] support::ExpectedVoid set_cursor_visible(bool visible) override {
        return terminal_.set_cursor_visible(visible);
    }
    [[nodiscard]] support::ExpectedVoid set_scroll_margins(std::size_t top_row, std::size_t bottom_row) override {
        return terminal_.set_scroll_margins(top_row, bottom_row);
    }
    [[nodiscard]] support::ExpectedVoid reset_scroll_margins() override { return terminal_.reset_scroll_margins(); }
    [[nodiscard]] support::ExpectedVoid begin_synchronized_update() override {
        return terminal_.begin_synchronized_update();
    }
    [[nodiscard]] support::ExpectedVoid end_synchronized_update() override {
        return terminal_.end_synchronized_update();
    }
    [[nodiscard]] support::ExpectedVoid set_title(std::string_view title) override {
        return terminal_.set_title(title);
    }
    [[nodiscard]] support::ExpectedVoid apply_cell_pixel_dimensions(cch::tui::CellPixelDimensions pixels) override {
        return terminal_.apply_cell_pixel_dimensions(pixels);
    }
    [[nodiscard]] support::ExpectedVoid set_progress(bool active) override { return terminal_.set_progress(active); }
    [[nodiscard]] support::ExpectedVoid set_program_status(const cch::tui::ProgramStatus& status) override {
        return terminal_.set_program_status(status);
    }
    [[nodiscard]] support::ExpectedVoid drain_input(
            std::chrono::milliseconds max_ms, std::chrono::milliseconds idle_ms) override {
        return terminal_.drain_input(max_ms, idle_ms);
    }

    [[nodiscard]] const std::vector<std::string>& sequences() const { return sequences_; }

private:
    cch::tui::VirtualTerminal& terminal_;
    std::vector<std::string> sequences_{};
};

/// The optional injected platform helper (pi's `NativeClipboard`): the image and
/// text a desktop provider would expose. Records whether its text was read, so a
/// test can prove the helper-command path short-circuited — or fell through to
/// the platform helper — before it.
class StubNativeClipboard final : public cch::tui::NativeClipboard {
public:
    [[nodiscard]] support::AsyncResult<cch::tui::NativeClipboardImageReadVariant> get_image(
            std::stop_token stop_token) override {
        if (stop_token.stop_requested()) {
            return support::AsyncResult<cch::tui::NativeClipboardImageReadVariant>{
                    std::unexpected(support::make_error(support::ErrorCode::Cancelled, "clipboard read cancelled"))};
        }
        return support::AsyncResult<cch::tui::NativeClipboardImageReadVariant>{
                cch::tui::NativeClipboardImageReadVariant{image}};
    }

    [[nodiscard]] support::AsyncResult<cch::tui::NativeClipboardTextReadVariant> get_text(
            std::stop_token stop_token) override {
        ++text_read_count;
        if (stop_token.stop_requested()) {
            return support::AsyncResult<cch::tui::NativeClipboardTextReadVariant>{
                    std::unexpected(support::make_error(support::ErrorCode::Cancelled, "clipboard read cancelled"))};
        }
        return support::AsyncResult<cch::tui::NativeClipboardTextReadVariant>{
                cch::tui::NativeClipboardTextReadVariant{text}};
    }

    std::optional<std::vector<std::uint8_t>> image{std::nullopt};
    std::optional<std::string> text{std::nullopt};
    std::size_t text_read_count{0};
};

[[nodiscard]] tests::ModelsSessionOptions session_options(
        const tests::TempWorkspace& workspace, std::shared_ptr<tests::ScriptedProvider> client) {
    tests::ModelsSessionOptions options;
    options.session_target = cch::coding_agent::InMemorySessionTarget{};
    options.workspace = workspace.path();
    options.models = cch::tests::models_from_provider(std::move(client));
    return options;
}

[[nodiscard]] support::AsyncResult<cch::coding_agent::CreateAgentSessionResult> create_session_async(
        tests::RuntimeFixture& runtime, tests::ModelsSessionOptions options) {
    auto models = std::move(options.models);
    cch::coding_agent::runtime::AgentSessionCreationRequest request = std::move(options);
    request.execution_runtime_target = runtime.make_target();
    return cch::coding_agent::create_agent_session_async(
            std::move(request), std::nullopt, cch::tests::cli_fake_overrides(std::move(models)));
}

[[nodiscard]] std::string visible_screen(const cch::tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] std::string read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// The environment the CLI observes on a desktop Linux session with an X11
/// display, resolving helpers only through the fake bin.
[[nodiscard]] ClipboardEnvironment display_environment(const FakeClipboardBin& bin) {
    ClipboardEnvironment environment{};
    environment.is_linux = true;
    environment.display = ":0";
    environment.search_path = bin.search_path();
    return environment;
}

/// One production-wired Native TUI session: the CLI clipboard assembly driven on
/// a virtual terminal, with an injected clipboard environment and fake helper
/// bin. The user-level config root is isolated so the developer's own settings
/// and keybindings never reach the run.
class ClipboardSession final {
public:
    /// Create the session, then assemble and start the run exactly as the CLI
    /// boot does (`assemble_interactive_session_run`).
    void start(ClipboardEnvironment environment, std::unique_ptr<cch::tui::NativeClipboard> native_helper = nullptr) {
        auto options = session_options(workspace, tests::make_scripted_fake_provider());
        created.emplace(runtime.run(create_session_async(runtime, std::move(options))));
        REQUIRE(*created);
        runtime_driver.emplace(runtime);
        terminal_double.emplace(terminal);
        boost::asio::co_spawn(io,
                cch::coding_agent::tui::run_interactive_mode(*terminal_double,
                        cch::cli::assemble_interactive_session_run(cch::cli::InitialMessageResult{},
                                cch::cli::CliConfig{},
                                cch::coding_agent::TuiMode::Regular,
                                /*runtime_root=*/nullptr,
                                /*shared_runtime=*/nullptr,
                                /*model_runtime_cli_fake=*/false,
                                error_stream,
                                cch::coding_agent::tui::BindExistingSession{(*created)->session.get()},
                                cch::cli::InteractiveClipboardInjection{
                                        .environment = environment,
                                        .native_helper = std::move(native_helper),
                                })),
                [this](std::exception_ptr exception, support::ExpectedVoid result) {
                    CHECK(exception == nullptr);
                    run_result.emplace(std::move(result));
                });
        tests::drain_ready(io);
    }

    /// Type `input` and pump until `done()`, keeping the run serviced.
    void type(std::string_view input) {
        REQUIRE(terminal.inject_input(std::string{input}));
        tests::drain_ready(io);
    }

    /// Ask the editor to submit the current line and wait for the agent message.
    void submit() {
        REQUIRE(terminal.inject_input("\r"));
        tests::drain_ready(io);
    }

    /// Wait until the visible screen shows `needle`.
    [[nodiscard]] bool wait_for_screen(std::string_view needle, std::chrono::milliseconds budget) {
        return tests::pump_until(
                io, [this, needle] { return visible_screen(terminal).find(needle) != std::string::npos; }, budget);
    }

    /// Leave the session the way a user does and wait for the run outcome.
    void exit_session(std::chrono::milliseconds budget = std::chrono::seconds{8}) {
        REQUIRE(terminal.inject_input("\x04"));
        REQUIRE(tests::pump_until(io, [this] { return run_result.has_value(); }, budget));
        CHECK(*run_result);
    }

    [[nodiscard]] std::string user_message_text(std::size_t index) const {
        const auto snapshot = (*created)->session->snapshot();
        REQUIRE(snapshot.agent_state.messages.size() > index);
        const auto* user = std::get_if<ai::UserMessage>(&snapshot.agent_state.messages[index]);
        REQUIRE(user != nullptr);
        return ai::text_from_user_message(*user);
    }

    [[nodiscard]] std::size_t message_count() const {
        return (*created)->session->snapshot().agent_state.messages.size();
    }

    tests::RuntimeFixture runtime{};
    tests::TempWorkspace workspace{};
    tests::TempWorkspace config_home{};
    tests::EnvVarGuard xdg_config_home{"XDG_CONFIG_HOME", config_home.path().string()};
    FakeClipboardBin bin{};
    boost::asio::io_context io{};
    std::ostringstream error_stream{};
    cch::tui::VirtualTerminal terminal{{.columns = 120, .rows = 18}};
    std::optional<Osc52RecordingTerminal> terminal_double{};
    std::optional<support::Expected<cch::coding_agent::CreateAgentSessionResult>> created{};
    std::optional<tests::RuntimeLoopDriver> runtime_driver{};
    std::optional<support::ExpectedVoid> run_result{};
    /// Declared last so a failing assertion still lets the run finish while its
    /// captured terminal and session are alive.
    tests::RunJoinGuard run_join_guard{io, [this] { return run_result.has_value(); }};
};

TEST_CASE("Native TUI /copy reports the X11 helper failure instead of fabricating an OSC 52 success",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    ClipboardSession session;
    session.bin.install({.name = "xclip", .exit_code = 1, .reads_stdin = true});
    session.bin.install({.name = "xsel", .exit_code = 1, .reads_stdin = true});
    session.start(display_environment(session.bin));

    session.type("hello\r");
    REQUIRE(session.wait_for_screen("fake: hello", std::chrono::seconds{10}));

    session.type("/copy\r");

    REQUIRE(session.wait_for_screen("Clipboard unavailable", std::chrono::seconds{5}));
    const auto screen = visible_screen(session.terminal);
    CHECK(screen.find("Clipboard unavailable: install `xclip` or `xsel`") != std::string::npos);
    CHECK(screen.find("Copied last agent message to clipboard") == std::string::npos);
    // Both X11 helpers were tried, and neither OSC 52 nor a success was invented
    // for a desktop session with a display (#9618).
    CHECK(session.bin.invocations() ==
            std::vector<std::string>{"xclip -selection clipboard fake: hello", "xsel --clipboard --input fake: hello"});
    CHECK(session.terminal_double->sequences().empty());

    session.exit_session();
}

TEST_CASE("Native TUI /copy in a remote session writes the OSC 52 sequence for the last agent message",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    ClipboardSession session;
    session.bin.install({.name = "xclip", .exit_code = 1, .reads_stdin = true});
    session.bin.install({.name = "xsel", .exit_code = 1, .reads_stdin = true});
    auto environment = display_environment(session.bin);
    environment.ssh_connection = "10.0.0.1 5555 10.0.0.2 22";
    session.start(environment);

    session.type("hello\r");
    REQUIRE(session.wait_for_screen("fake: hello", std::chrono::seconds{10}));

    session.type("/copy\r");

    REQUIRE(session.wait_for_screen("Copied last agent message to clipboard", std::chrono::seconds{5}));
    REQUIRE(session.terminal_double->sequences().size() == 1);
    const auto& sequence = session.terminal_double->sequences()[0];
    // pi `emitOsc52`: the exact `\x1b]52;c;<base64 of the UTF-8 text>\x07`.
    CHECK(sequence == std::string{"\x1b]52;c;ZmFrZTogaGVsbG8=\x07"});
    CHECK(tests::bytes_as_string(tests::decode_base64(std::string_view{sequence}.substr(7, sequence.size() - 8))) ==
            "fake: hello");

    session.exit_session();
}

TEST_CASE("Native TUI paste reads the Linux clipboard helper the CLI assembly registered",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    ClipboardSession session;
    session.bin.install({.name = "xclip", .output = "clipboard text"});
    session.start(display_environment(session.bin));

    session.type("AB\x1b[D\x16");
    REQUIRE(session.bin.invocations() == std::vector<std::string>{"xclip -selection clipboard -out"});
    tests::drain_ready(session.io);
    session.submit();
    REQUIRE(session.wait_for_screen("fake: Aclipboard textB", std::chrono::seconds{10}));

    CHECK(session.user_message_text(1) == "Aclipboard textB");
    session.exit_session();
}

TEST_CASE("Native TUI paste tells an empty helper result apart from a failed helper",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    ClipboardSession session;
    session.bin.install({.name = "xclip", .exit_code = 1});
    session.bin.install({.name = "xsel", .exit_code = 1});

    auto helper = std::make_unique<StubNativeClipboard>();
    helper->text = std::string{"from native helper"};
    const auto* stub = helper.get();
    session.start(display_environment(session.bin), std::move(helper));

    SECTION("failing helpers fall through to the platform helper") {
        session.type("XY\x16");
        REQUIRE(tests::pump_until(session.io, [&] { return stub->text_read_count == 1; }, std::chrono::seconds{5}));
        session.submit();
        REQUIRE(session.wait_for_screen("fake: XYfrom native helper", std::chrono::seconds{10}));

        CHECK(session.user_message_text(1) == "XYfrom native helper");
        CHECK(session.bin.invocations() ==
                std::vector<std::string>{"xclip -selection clipboard -out", "xsel --clipboard --output"});
    }

    SECTION("a successful empty helper result is an empty read, not a fallback") {
        session.bin.install({.name = "xclip"});
        session.type("XY\x16");
        session.submit();
        REQUIRE(session.wait_for_screen("fake: XY", std::chrono::seconds{10}));

        // The helper exited 0 with no output: pi's empty read stops there, so
        // the platform helper is never consulted and nothing is inserted.
        CHECK(stub->text_read_count == 0);
        CHECK(session.user_message_text(1) == "XY");
        CHECK(session.bin.invocations() == std::vector<std::string>{"xclip -selection clipboard -out"});
    }

    session.exit_session();
}

TEST_CASE("Native TUI paste keeps an image from the injected platform helper",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    ClipboardSession session;
    const auto source_bytes = tests::decode_base64(tests::kTinyPngBase64);
    auto helper = std::make_unique<StubNativeClipboard>();
    helper->image = source_bytes;
    session.start(display_environment(session.bin), std::move(helper));

    session.type("\x16");
    tests::drain_ready(session.io);
    session.submit();
    REQUIRE(session.wait_for_screen("fake: /", std::chrono::seconds{10}));

    // The image becomes one supported temporary path, and the helper-command
    // text path is never used.
    const auto pasted = session.user_message_text(1);
    std::istringstream paths{pasted};
    std::filesystem::path inserted_path;
    REQUIRE(static_cast<bool>(paths >> inserted_path));
    CHECK(inserted_path.extension() == ".png");
    CHECK(std::filesystem::exists(inserted_path));
    CHECK(read_binary_file(inserted_path) == tests::bytes_as_string(source_bytes));
    CHECK(session.bin.invocations().empty());

    session.exit_session();

    std::error_code remove_error;
    CHECK(std::filesystem::remove(inserted_path, remove_error));
    CHECK_FALSE(remove_error);
}

TEST_CASE("Native TUI Close cancels the in-flight clipboard helper read",
        "[coding_agent][tui][clipboard][issue993][spec]") {
    ClipboardSession session;
    session.bin.install({.name = "xclip", .delay_seconds = 30});
    session.start(display_environment(session.bin));

    // Paste starts a helper that will not finish on its own.
    session.type("\x16");
    REQUIRE(tests::pump_until(session.io, [&] { return !session.bin.invocations().empty(); }, std::chrono::seconds{5}));

    const auto close_start = Clock::now();
    REQUIRE(session.terminal.inject_input("\x04"));
    REQUIRE(tests::pump_until(session.io, [&] { return session.run_result.has_value(); }, std::chrono::seconds{8}));
    const auto close_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - close_start);

    CHECK(*session.run_result);
    // The read's own 5000 ms timeout would still be pending: the fast, clean exit
    // is the cancellation killing and reaping the live helper, and the cancelled
    // paste inserted nothing.
    CHECK(close_elapsed < std::chrono::seconds{2});
    CHECK(session.bin.invocations().size() == 1);
    CHECK(session.message_count() <= 1);
}

} // namespace
} // namespace cch::coding_agent::tui
