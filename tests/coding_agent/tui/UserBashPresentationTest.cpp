#include "coding_agent/tui/UserBashPresentation.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Loader.hpp>
#include "coding_agent/tui/Theme.hpp"
#include "util/Redactor.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
namespace runtime = cch::coding_agent::runtime;
using coding_agent::tui::BashBlockView;
using coding_agent::tui::LiveTheme;
using coding_agent::tui::PendingUserBashPresentation;
using coding_agent::tui::render_bash_block;
using coding_agent::tui::user_bash_block_view;

namespace {

[[nodiscard]] LiveTheme test_theme() {
    return LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        cch::tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] cch::tui::KeybindingRegistry test_keybindings() {
    return cch::tui::KeybindingRegistry(
        {
            cch::tui::EffectiveKeybinding{
                .id = "app.tools.expand",
                .keys = {"ctrl+o"},
            },
            cch::tui::EffectiveKeybinding{
                .id = "app.interrupt",
                .keys = {"esc"},
            },
        },
        cch::tui::KeybindingPlatform::Linux);
}

[[nodiscard]] bool any_line_contains(
    const std::vector<std::string>& lines,
    std::string_view needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] runtime::UserBashProgress running_progress() {
    return runtime::UserBashProgress{
        .command = "echo hi",
        .output = "hi\n",
        .exclude_from_context = false,
        .awaiting_commitment = false,
        .exit_code = std::nullopt,
        .cancelled = false,
        .truncated = false,
        .full_output_path = std::nullopt,
    };
}

} // namespace

TEST_CASE(
    "user_bash_block_view owns the running invariant for live progress",
    "[coding_agent][tui][user-bash-presentation]") {
    runtime::UserBashProgress progress{
        .command = "make -j",
        .output = "building\n",
        .exclude_from_context = true,
        .awaiting_commitment = false,
        .exit_code = std::nullopt,
        .cancelled = false,
        .truncated = true,
        .full_output_path = std::string{"/tmp/full.txt"},
    };

    auto view = user_bash_block_view(progress);
    CHECK(view.command == "make -j");
    CHECK(view.output == "building\n");
    CHECK(view.exclude_from_context);
    CHECK(view.running);
    CHECK_FALSE(view.exit_code.has_value());
    CHECK(view.truncated);
    CHECK(view.full_output_path == std::string{"/tmp/full.txt"});

    progress.awaiting_commitment = true;
    progress.exit_code = 0;
    auto committed_form = user_bash_block_view(progress);
    CHECK_FALSE(committed_form.running);
    CHECK(committed_form.exit_code == 0);
}

TEST_CASE(
    "user_bash_block_view maps a committed message as never running",
    "[coding_agent][tui][user-bash-presentation]") {
    const ai::BashExecutionMessage message{
        .command = "ls",
        .output = "README.md\n",
        .exit_code = 2,
        .cancelled = true,
        .truncated = false,
        .full_output_path = std::nullopt,
        .exclude_from_context = true,
    };

    const auto view = user_bash_block_view(message);
    CHECK(view.command == "ls");
    CHECK(view.output == "README.md\n");
    CHECK(view.exclude_from_context);
    CHECK_FALSE(view.running);
    CHECK(view.exit_code == 2);
    CHECK(view.cancelled);
}

TEST_CASE(
    "render_bash_block withholds status lines while running",
    "[coding_agent][tui][user-bash-presentation]") {
    const auto theme = test_theme();
    const auto keybindings = test_keybindings();

    BashBlockView view{
        .command = "false",
        .output = "boom\n",
        .exclude_from_context = false,
        .running = true,
        .exit_code = 1,
        .cancelled = true,
        .truncated = false,
        .full_output_path = std::nullopt,
    };

    auto running = render_bash_block(theme, keybindings, view, false, 80);
    REQUIRE(running);
    CHECK(any_line_contains(*running, "$ false"));
    CHECK_FALSE(any_line_contains(*running, "(cancelled)"));
    CHECK_FALSE(any_line_contains(*running, "(exit 1)"));

    view.running = false;
    auto settled = render_bash_block(theme, keybindings, view, false, 80);
    REQUIRE(settled);
    CHECK(any_line_contains(*settled, "(cancelled)"));
}

TEST_CASE(
    "render_bash_block reports a non-zero exit and the truncation spill path",
    "[coding_agent][tui][user-bash-presentation]") {
    const auto theme = test_theme();
    const auto keybindings = test_keybindings();

    const BashBlockView view{
        .command = "cat big",
        .output = "tail\n",
        .exclude_from_context = false,
        .running = false,
        .exit_code = 3,
        .cancelled = false,
        .truncated = true,
        .full_output_path = std::string{"/tmp/spill.txt"},
    };

    auto rendered = render_bash_block(theme, keybindings, view, false, 80);
    REQUIRE(rendered);
    CHECK(any_line_contains(*rendered, "(exit 3)"));
    CHECK(any_line_contains(*rendered, "Output truncated. Full output: /tmp/spill.txt"));
}

TEST_CASE(
    "render_bash_block shows command and output raw without render-time redaction",
    "[coding_agent][tui][user-bash-presentation][issue99]") {
    const auto theme = test_theme();
    const auto keybindings = test_keybindings();
    const std::string secret = "sk-abcdefghijklmnopqrstuvwxyz123456";

    const BashBlockView view{
        .command = "printf api_key=" + secret,
        .output = "token=" + secret + "\n",
        .exclude_from_context = false,
        .running = false,
        .exit_code = 0,
        .cancelled = false,
        .truncated = false,
        .full_output_path = std::nullopt,
    };

    auto rendered = render_bash_block(theme, keybindings, view, false, 80);
    REQUIRE(rendered);
    CHECK(any_line_contains(*rendered, "$ printf api_key=" + secret));
    CHECK(any_line_contains(*rendered, "token=" + secret));
    CHECK_FALSE(any_line_contains(*rendered, util::kRedactionMarker));
}

TEST_CASE(
    "render_bash_block bounds the collapsed preview to the last 20 logical lines",
    "[coding_agent][tui][user-bash-presentation]") {
    const auto theme = test_theme();
    const auto keybindings = test_keybindings();

    std::string output;
    for (int line = 1; line <= 25; ++line) {
        output += std::format("line {}\n", line);
    }

    const BashBlockView view{
        .command = "seq",
        .output = output,
        .exclude_from_context = false,
        .running = false,
        .exit_code = std::nullopt,
        .cancelled = false,
        .truncated = false,
        .full_output_path = std::nullopt,
    };

    auto collapsed = render_bash_block(theme, keybindings, view, false, 80);
    REQUIRE(collapsed);
    // A trailing newline yields a trailing empty logical line, so 26 logical
    // lines hide 6 behind the 20-line preview.
    CHECK(any_line_contains(*collapsed, "... 6 more lines ("));
    CHECK(any_line_contains(*collapsed, "ctrl+o"));
    CHECK(any_line_contains(*collapsed, "to expand)"));
    CHECK(any_line_contains(*collapsed, "line 25"));
    CHECK_FALSE(any_line_contains(*collapsed, "line 5\n"));

    auto expanded = render_bash_block(theme, keybindings, view, true, 80);
    REQUIRE(expanded);
    CHECK(any_line_contains(*expanded, "line 1"));
    CHECK_FALSE(any_line_contains(*expanded, "more lines"));
    CHECK(any_line_contains(*expanded, "to collapse)"));
}

TEST_CASE(
    "PendingUserBashPresentation derives the loader lifecycle from progress",
    "[coding_agent][tui][user-bash-presentation]") {
    const auto theme = test_theme();
    const auto keybindings = test_keybindings();

    std::size_t loaders_created = 0;
    PendingUserBashPresentation pending([&loaders_created](bool) {
        ++loaders_created;
        cch::tui::LoaderOptions options;
        options.message = "Running... (esc to cancel)";
        return std::make_unique<cch::tui::Loader>(std::move(options));
    });

    CHECK_FALSE(pending.active());
    auto empty = pending.render(theme, keybindings, false, 80);
    REQUIRE(empty);
    CHECK(empty->empty());

    pending.update(running_progress());
    CHECK(pending.active());
    CHECK(loaders_created == 1);

    auto running = pending.render(theme, keybindings, false, 80);
    REQUIRE(running);
    CHECK(any_line_contains(*running, "$ echo hi"));
    CHECK(any_line_contains(*running, "Running... (esc to cancel)"));

    // Further running progress reuses the existing loader.
    auto more = running_progress();
    more.output += "again\n";
    pending.update(std::move(more));
    CHECK(loaders_created == 1);

    // A completed execution awaiting commitment stops the loader and shows
    // the final block form.
    auto settled = running_progress();
    settled.awaiting_commitment = true;
    settled.exit_code = 0;
    pending.update(std::move(settled));
    auto committed_form = pending.render(theme, keybindings, false, 80);
    REQUIRE(committed_form);
    CHECK(any_line_contains(*committed_form, "$ echo hi"));
    CHECK_FALSE(any_line_contains(*committed_form, "Running..."));

    pending.clear();
    CHECK_FALSE(pending.active());
    auto cleared = pending.render(theme, keybindings, false, 80);
    REQUIRE(cleared);
    CHECK(cleared->empty());

    // A later execution creates a fresh loader.
    pending.update(running_progress());
    CHECK(loaders_created == 2);
}
