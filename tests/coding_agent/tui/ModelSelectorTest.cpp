// The model selector (pi `model-selector.ts`): renders the model list with
// fuzzy search and the all/scoped scope toggle, selects through the callback,
// cancels on Escape/Ctrl+C, and refreshes availability in the background.
// Component-level coverage over a real ModelRuntime with dummy-only
// models.json values — no live credentials, no network validation.

#include "coding_agent/tui/ModelSelector.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>
#include "support/AsyncResultBridge.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

constexpr std::string_view kThreeKeyedProviders = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "name": "Alpha One"}, {"id": "alpha-2"}]
    },
    "beta": {
      "baseUrl": "https://beta.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-beta-key",
      "models": [{"id": "beta-1", "name": "Beta One"}]
    }
  }
})";

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) ++index;
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) {
        text.append(strip_ansi(line));
        text.push_back('\n');
    }
    return text;
}

/// A runtime over a temp Agent Config Directory with dummy-only models.json.
/// Ambient KIMI_API_KEY is unset so the built-in kimi-coding provider never
/// resolves as configured unless a test says so.
struct RuntimeFixture {
    tests::TempWorkspace agent_dir;
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};
    std::shared_ptr<coding_agent::ModelRuntime> runtime;

    explicit RuntimeFixture(std::string_view models_json = kThreeKeyedProviders) {
        kimi_guard.unset();
        {
            std::ofstream out(agent_dir.path() / "models.json", std::ios::binary);
            out << models_json;
        }
        auto created = coding_agent::ModelRuntime::create({
            .agent_dir = agent_dir.path(),
        });
        REQUIRE(created);
        runtime = std::move(*created);
    }

    /// Populate the availability snapshot before the component renders.
    void prime(boost::asio::io_context& io) {
        auto result = std::optional<support::ExpectedVoid>{};
        boost::asio::co_spawn(
                io,
                [runtime = runtime]() -> boost::asio::awaitable<support::ExpectedVoid> {
                    auto available = co_await support::detail::await_async_result(runtime->get_available());
                    if (!available) {
                        co_return std::unexpected(available.error());
                    }
                    co_return support::ExpectedVoid{};
                },
                [&](std::exception_ptr exception, support::ExpectedVoid outcome) {
                    REQUIRE(exception == nullptr);
                    result.emplace(std::move(outcome));
                });
        io.run();
        REQUIRE(result.has_value());
        REQUIRE(*result);
    }
};

} // namespace

TEST_CASE(
    "ModelSelector renders the scope text, search input, and sorted list with the current marker",
    "[coding_agent][tui][model-selector][issue407]") {
    RuntimeFixture fixture;
    boost::asio::io_context io;
    fixture.prime(io);

    auto theme = test_theme();
    const auto current = fixture.runtime->model("alpha", "alpha-2");
    REQUIRE(current.has_value());
    std::optional<ai::Model> selected;
    std::size_t cancellations = 0;
    auto selector = std::make_shared<coding_agent::tui::ModelSelectorComponent>(
        theme,
        test_keybindings(),
        &*current,
        fixture.runtime,
        io.get_executor(),
        std::vector<cch::coding_agent::ScopedModel>{},
        [&selected](ai::Model model) { selected = std::move(model); },
        [&cancellations] { ++cancellations; },
        [&io] {
            // The test drives rendering directly; invalidations are no-ops.
            (void)io;
        });

    // No scoped models → the provider hint renders instead of the scope
    // toggle, and the current model sorts first (pi `sortModels`).
    const auto rendered = selector->render(70);
    REQUIRE(rendered);
    const auto screen = join_lines(rendered->lines);
    // The hint renders wrapped at narrow widths; assert the leading part.
    CHECK(screen.find("Only showing models from configured providers.") !=
        std::string::npos);
    CHECK(screen.find("Refreshing model catalogs…") != std::string::npos);
    const auto current_position = screen.find("alpha-2 [alpha]");
    const auto other_position = screen.find("alpha-1 [alpha]");
    REQUIRE(current_position != std::string::npos);
    REQUIRE(other_position != std::string::npos);
    CHECK(current_position < other_position);
    CHECK(screen.find("beta-1 [beta]") != std::string::npos);
    // The selected item's name line renders below the list (pi).
    CHECK(screen.find("Model Name:") != std::string::npos);

    // The background refresh completes and reports the refreshed status.
    if (io.stopped()) io.restart();
    while (io.poll() != 0) {
    }
    const auto refreshed = selector->render(70);
    REQUIRE(refreshed);
    const auto refreshed_screen = join_lines(refreshed->lines);
    CHECK(refreshed_screen.find("Model catalogs refreshed.") != std::string::npos);
    CHECK_FALSE(selected.has_value());
    CHECK(cancellations == 0);
}

TEST_CASE(
    "ModelSelector fuzzy search filters, moves the selection to the top match, and selects on Enter",
    "[coding_agent][tui][model-selector][issue407]") {
    RuntimeFixture fixture;
    boost::asio::io_context io;
    fixture.prime(io);

    auto theme = test_theme();
    std::optional<ai::Model> selected;
    auto selector = std::make_shared<coding_agent::tui::ModelSelectorComponent>(
        theme,
        test_keybindings(),
        nullptr,
        fixture.runtime,
        io.get_executor(),
        std::vector<cch::coding_agent::ScopedModel>{},
        [&selected](ai::Model model) { selected = std::move(model); },
        [] {},
        [&io] { (void)io; });

    // Type a search query: provider-prefixed text ranks before bare ids.
    selector->handle_input(tui::KeyEvent{.key = "b"});
    selector->handle_input(tui::KeyEvent{.key = "e"});
    {
        const auto rendered = selector->render(70);
        REQUIRE(rendered);
        const auto screen = join_lines(rendered->lines);
        CHECK(screen.find("beta-1 [beta]") != std::string::npos);
        CHECK(screen.find("No matching models") == std::string::npos);
    }
    selector->handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(selected.has_value());
    CHECK(selected->provider == "beta");
    CHECK(selected->id == "beta-1");

    // A query with no matches renders the empty state.
    auto empty_selector = std::make_shared<coding_agent::tui::ModelSelectorComponent>(
        theme,
        test_keybindings(),
        nullptr,
        fixture.runtime,
        io.get_executor(),
        std::vector<cch::coding_agent::ScopedModel>{},
        [](ai::Model) -> support::ExpectedVoid { return {}; },
        [] {},
        [&io] { (void)io; });
    empty_selector->handle_input(tui::KeyEvent{.key = "z"});
    empty_selector->handle_input(tui::KeyEvent{.key = "z"});
    {
        const auto rendered = empty_selector->render(70);
        REQUIRE(rendered);
        CHECK(join_lines(rendered->lines).find("No matching models") != std::string::npos);
    }
}

TEST_CASE(
    "ModelSelector up/down wrap and Escape/Ctrl+C cancel",
    "[coding_agent][tui][model-selector][issue407]") {
    RuntimeFixture fixture;
    boost::asio::io_context io;
    fixture.prime(io);

    auto theme = test_theme();
    std::size_t cancellations = 0;
    auto selector = std::make_shared<coding_agent::tui::ModelSelectorComponent>(
        theme,
        test_keybindings(),
        nullptr,
        fixture.runtime,
        io.get_executor(),
        std::vector<cch::coding_agent::ScopedModel>{},
        [](ai::Model) -> support::ExpectedVoid { return {}; },
        [&cancellations] { ++cancellations; },
        [&io] { (void)io; });

    // Down from the top, then wrap up to the last item.
    selector->handle_input(tui::KeyEvent{.key = "down"});
    {
        const auto rendered = selector->render(70);
        REQUIRE(rendered);
        CHECK(join_lines(rendered->lines).find("→ alpha-2") != std::string::npos);
    }
    selector->handle_input(tui::KeyEvent{.key = "up"});
    selector->handle_input(tui::KeyEvent{.key = "up"});
    {
        const auto rendered = selector->render(70);
        REQUIRE(rendered);
        CHECK(join_lines(rendered->lines).find("→ beta-1") != std::string::npos);
    }

    selector->handle_input(tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 1);
    selector->handle_input(tui::KeyEvent{.key = "c", .ctrl = true});
    CHECK(cancellations == 2);
}

TEST_CASE(
    "ModelSelector toggles the all/scoped scope on Tab when scoped models exist",
    "[coding_agent][tui][model-selector][issue407]") {
    RuntimeFixture fixture;
    boost::asio::io_context io;
    fixture.prime(io);

    auto theme = test_theme();
    const auto scoped_model = fixture.runtime->model("alpha", "alpha-1");
    REQUIRE(scoped_model.has_value());
    auto selector = std::make_shared<coding_agent::tui::ModelSelectorComponent>(
        theme,
        test_keybindings(),
        nullptr,
        fixture.runtime,
        io.get_executor(),
        std::vector<cch::coding_agent::ScopedModel>{coding_agent::ScopedModel{.model = *scoped_model}},
        [](ai::Model) -> support::ExpectedVoid { return {}; },
        [] {},
        [&io] { (void)io; });

    // The initial scope is "scoped" when scoped models exist (pi), with the
    // Tab hint; only the scoped model is listed.
    {
        const auto rendered = selector->render(70);
        REQUIRE(rendered);
        const auto screen = join_lines(rendered->lines);
        CHECK(screen.find("Scope:") != std::string::npos);
        CHECK(screen.find("(all/scoped)") != std::string::npos);
        CHECK(screen.find("alpha-1 [alpha]") != std::string::npos);
        CHECK(screen.find("beta-1") == std::string::npos);
    }

    // Tab switches to the all scope.
    selector->handle_input(tui::KeyEvent{.key = "tab"});
    {
        const auto rendered = selector->render(70);
        REQUIRE(rendered);
        const auto screen = join_lines(rendered->lines);
        CHECK(screen.find("beta-1 [beta]") != std::string::npos);
    }
    // Tab again returns to the scoped scope.
    selector->handle_input(tui::KeyEvent{.key = "tab"});
    {
        const auto rendered = selector->render(70);
        REQUIRE(rendered);
        CHECK(join_lines(rendered->lines).find("beta-1") == std::string::npos);
    }
}

/// Every emitted line must fit the render width bound exactly: the TUI render
/// path asserts each line's visible width <= the bound and aborts the whole
/// app on a single over-wide line (issue #426). The model selectors used to
/// emit raw, untruncated lines — this pins the width-boundary behavior.
static void check_all_lines_bounded(const tui::RenderResult& rendered, std::size_t width) {
    REQUIRE_FALSE(rendered.lines.empty());
    for (const auto& line : rendered.lines) {
        const auto visible = cch::tui::visible_width(strip_ansi(line));
        CHECK(visible <= width);
    }
}

TEST_CASE(
    "ModelSelector never emits a line wider than the render width",
    "[coding_agent][tui][model-selector][issue426]") {
    RuntimeFixture fixture;
    boost::asio::io_context io;
    fixture.prime(io);

    auto theme = test_theme();
    auto selector = std::make_shared<coding_agent::tui::ModelSelectorComponent>(
        theme,
        test_keybindings(),
        nullptr,
        fixture.runtime,
        io.get_executor(),
        std::vector<cch::coding_agent::ScopedModel>{},
        [](ai::Model) -> support::ExpectedVoid { return {}; },
        [] {},
        [&io] { (void)io; });

    // A narrow width that any raw (untruncated) model line would exceed; the
    // reported defect reproduced at widths 20-80.
    for (const std::size_t width : {10ul, 20ul, 30ul}) {
        const auto rendered = selector->render(width);
        REQUIRE(rendered);
        check_all_lines_bounded(*rendered, width);
    }

    // Also verify the longest raw line is bounded: probe at a width where the
    // alpha-1/alpha-2 model lines would naturally run long.
    {
        const auto rendered = selector->render(6);
        REQUIRE(rendered);
        check_all_lines_bounded(*rendered, 6);
    }
}
