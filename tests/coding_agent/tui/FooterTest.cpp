#include "coding_agent/tui/Footer.hpp"
#include "coding_agent/tui/FooterDataProvider.hpp"
#include "coding_agent/tui/StatusIndicator.hpp"

#include "support/TempWorkspace.hpp"

#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "coding_agent/tui/Theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

using namespace cch;

namespace {

struct FooterFixture {
    coding_agent::tui::LiveTheme theme{
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::Xterm256};
    coding_agent::tui::Footer footer{theme};
};

/// Render a footer and strip ANSI for readable checks.
[[nodiscard]] std::array<std::string, 2> rendered_lines(
    coding_agent::tui::Footer& footer,
    std::size_t width) {
    auto rendered = footer.render(width);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 2);
    return {
        tui::strip_terminal_sequences(rendered->lines[0]),
        tui::strip_terminal_sequences(rendered->lines[1]),
    };
}

} // namespace

TEST_CASE("Footer formatTokens matches pi's compact formatting", "[coding_agent][tui][footer][issue411]") {
    CHECK(coding_agent::tui::format_tokens(0) == "0");
    CHECK(coding_agent::tui::format_tokens(999) == "999");
    CHECK(coding_agent::tui::format_tokens(1000) == "1.0k");
    CHECK(coding_agent::tui::format_tokens(1234) == "1.2k");
    CHECK(coding_agent::tui::format_tokens(9999) == "10.0k");
    CHECK(coding_agent::tui::format_tokens(10000) == "10k");
    CHECK(coding_agent::tui::format_tokens(34567) == "35k");
    CHECK(coding_agent::tui::format_tokens(999999) == "1000k");
    CHECK(coding_agent::tui::format_tokens(1000000) == "1.0M");
    CHECK(coding_agent::tui::format_tokens(1234567) == "1.2M");
    CHECK(coding_agent::tui::format_tokens(9999999) == "10.0M");
    CHECK(coding_agent::tui::format_tokens(10000000) == "10M");
    CHECK(coding_agent::tui::format_tokens(12000000) == "12M");
}

TEST_CASE("Footer formatCwdForFooter replaces the home prefix with ~", "[coding_agent][tui][footer][issue411]") {
    const std::filesystem::path home{"/home/user"};
    CHECK(coding_agent::tui::format_cwd_for_footer("/home/user", home) == "~");
    CHECK(coding_agent::tui::format_cwd_for_footer("/home/user/projects/x", home) == "~/projects/x");
    CHECK(coding_agent::tui::format_cwd_for_footer("/home/other", home) == "/home/other");
    CHECK(coding_agent::tui::format_cwd_for_footer("/home/user2", home) == "/home/user2");
    CHECK(coding_agent::tui::format_cwd_for_footer("/tmp", std::nullopt) == "/tmp");
}

TEST_CASE(
    "Footer renders pi's two lines: dim pwd with branch and the stats line with model on the right",
    "[coding_agent][tui][footer][issue411]") {
    auto footer = FooterFixture{};
    coding_agent::tui::FooterData data;
    data.cwd = "/home/user/projects/harness";
    data.git_branch = "main";
    data.input = 1234;
    data.output = 567;
    data.cache_read = 10000;
    data.cache_write = 2000;
    data.cache_hit_rate = 83.3333;
    data.cost = 0.0123;
    data.context_tokens = 4567;
    data.context_window = 200000;
    data.auto_compact_enabled = true;
    data.model_id = "claude-sonnet";
    data.model_reasoning = true;
    data.thinking_level = "medium";
    data.available_provider_count = 1;
    footer.footer.set_data(std::move(data));

    const auto [pwd_line, stats_line] = rendered_lines(footer.footer, 80);
    CHECK(pwd_line.starts_with("/home/user/projects/harness (main)"));
    CHECK(stats_line.find("\xe2\x86\x91" "1.2k") != std::string::npos);
    CHECK(stats_line.find("\xe2\x86\x93" "567") != std::string::npos);
    CHECK(stats_line.find("R10k") != std::string::npos);
    CHECK(stats_line.find("W2.0k") != std::string::npos);
    CHECK(stats_line.find("CH83.3%") != std::string::npos);
    CHECK(stats_line.find("$0.012") != std::string::npos);
    CHECK(stats_line.find("2.3%/200k (auto)") != std::string::npos);
    // The model with its thinking level is right-aligned.
    CHECK(stats_line.find("claude-sonnet \xc2\xb7 medium") != std::string::npos);
    // The stats line ends with the model (right-aligned) and is 80 wide.
    CHECK(tui::visible_width(stats_line) == 80);
    CHECK(stats_line.find("claude-sonnet \xc2\xb7 medium") > stats_line.find("CH83.3%"));
}

TEST_CASE("Footer omits zero stats parts like pi", "[coding_agent][tui][footer][issue411]") {
    auto fixture = FooterFixture{};
    auto& footer = fixture.footer;
    coding_agent::tui::FooterData data;
    data.cwd = "/tmp";
    data.context_window = 100000;
    data.model_id = "deepseek-chat";
    footer.set_data(std::move(data));

    const auto [pwd_line, stats_line] = rendered_lines(footer, 80);
    CHECK(pwd_line.starts_with("/tmp"));
    CHECK(stats_line.find("\xe2\x86\x91") == std::string::npos);
    CHECK(stats_line.find("$") == std::string::npos);
    CHECK(stats_line.find("?/100k (auto)") != std::string::npos);
    CHECK(stats_line.find("deepseek-chat") != std::string::npos);
}

TEST_CASE("Footer shows the kimi subscription marker and the provider prefix", "[coding_agent][tui][footer][issue411]") {
    auto fixture = FooterFixture{};
    auto& footer = fixture.footer;
    coding_agent::tui::FooterData data;
    data.cwd = "/tmp";
    data.cost = 0.5;
    data.using_subscription = true;
    data.context_window = 100000;
    data.provider = "kimi-coding";
    data.model_id = "kimi-k2";
    data.available_provider_count = 2;
    footer.set_data(std::move(data));

    const auto [pwd_line, stats_line] = rendered_lines(footer, 80);
    CHECK(pwd_line.starts_with("/tmp"));
    CHECK(stats_line.find("$0.500 (sub)") != std::string::npos);
    CHECK(stats_line.find("(kimi-coding) kimi-k2") != std::string::npos);
}

TEST_CASE("Footer colors the context percent by threshold and dims the stats", "[coding_agent][tui][footer][issue411]") {
    auto fixture = FooterFixture{};
    auto& footer = fixture.footer;
    coding_agent::tui::FooterData warning_data;
    warning_data.cwd = "/tmp";
    warning_data.context_tokens = 150000;
    warning_data.context_window = 200000;
    warning_data.model_id = "m";
    footer.set_data(warning_data);
    auto rendered = footer.render(80);
    REQUIRE(rendered);
    // 75% -> warning color.
    const auto warning_color = coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::Xterm256)
        .foreground(coding_agent::tui::ThemeToken::Warning, "x");
    CHECK(rendered->lines[1].find(warning_color.substr(0, warning_color.size() - 6)) != std::string::npos);

    coding_agent::tui::FooterData error_data;
    error_data.cwd = "/tmp";
    error_data.context_tokens = 190000;
    error_data.context_window = 200000;
    error_data.model_id = "m";
    footer.set_data(error_data);
    rendered = footer.render(80);
    REQUIRE(rendered);
    const auto error_color = coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::Xterm256)
        .foreground(coding_agent::tui::ThemeToken::Error, "x");
    CHECK(rendered->lines[1].find(error_color.substr(0, error_color.size() - 6)) != std::string::npos);
    // The pwd line is dim.
    const auto dim_color = coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::Xterm256)
        .foreground(coding_agent::tui::ThemeToken::Dim, "x");
    CHECK(rendered->lines[0].find(dim_color.substr(0, dim_color.size() - 6)) != std::string::npos);
}

TEST_CASE("Footer truncates the pwd and right side to the width", "[coding_agent][tui][footer][issue411]") {
    auto fixture = FooterFixture{};
    auto& footer = fixture.footer;
    coding_agent::tui::FooterData data;
    data.cwd = "/very/long/working/directory/path/that/exceeds/the/footer/width";
    data.context_window = 100000;
    data.model_id = "a-very-long-model-name-that-cannot-possibly-fit";
    footer.set_data(std::move(data));

    const auto [pwd_line, stats_line] = rendered_lines(footer, 40);
    CHECK(pwd_line.size() == 40);
    CHECK(pwd_line.find("...") != std::string::npos);
    CHECK(stats_line.size() == 40);
    // The model is truncated to fit the remaining width.
    CHECK(stats_line.find("a-very-long-model-name") != std::string::npos);
}

TEST_CASE("FooterDataProvider resolves the git branch from HEAD metadata", "[coding_agent][tui][footer][issue411]") {
    tests::TempWorkspace workspace;
    const auto git_dir = workspace.path() / "repo" / ".git";
    std::filesystem::create_directories(git_dir);
    {
        std::ofstream head(git_dir / "HEAD");
        head << "ref: refs/heads/feature-branch\n";
    }

    coding_agent::tui::FooterDataProvider provider(workspace.path() / "repo" / "src");
    CHECK(provider.git_branch() == std::optional<std::string>{"feature-branch"});
    CHECK(provider.git_branch() == std::optional<std::string>{"feature-branch"});

    // Detached HEAD (a raw commit id).
    {
        std::ofstream head(git_dir / "HEAD");
        head << "0123456789abcdef0123456789abcdef01234567\n";
    }
    // The cache TTL keeps the old value briefly; force a re-read through a
    // new provider to avoid timing dependence.
    coding_agent::tui::FooterDataProvider detached(workspace.path() / "repo" / "src");
    CHECK(detached.git_branch() == std::optional<std::string>{"detached"});
}

TEST_CASE("FooterDataProvider handles worktrees and missing repos", "[coding_agent][tui][footer][issue411]") {
    tests::TempWorkspace workspace;
    const auto repo = workspace.path() / "repo";
    const auto worktree = workspace.path() / "worktree";
    const auto git_dir = repo / ".git";
    std::filesystem::create_directories(git_dir);
    {
        std::ofstream head(git_dir / "HEAD");
        head << "ref: refs/heads/main\n";
    }
    std::filesystem::create_directories(worktree);
    {
        std::ofstream gitfile(worktree / ".git");
        gitfile << "gitdir: " << (git_dir.string()) << "\n";
    }

    coding_agent::tui::FooterDataProvider worktree_provider(worktree);
    CHECK(worktree_provider.git_branch() == std::optional<std::string>{"main"});

    // Outside any repo: no branch.
    coding_agent::tui::FooterDataProvider outside(workspace.path() / "elsewhere");
    CHECK_FALSE(outside.git_branch().has_value());
}

TEST_CASE("Status indicator messages match pi's wording", "[coding_agent][tui][footer][issue411]") {
    cch::tui::KeybindingDefinition definition{
        .id = "app.interrupt",
        .default_keys = {"escape"},
        .description = "Cancel or abort",
        .category = "Application",
    };
    auto resolved = cch::tui::resolve_keybindings(
        cch::tui::KeybindingResolutionRequest{
            .definitions = {definition},
            .platform = cch::tui::KeybindingPlatform::Linux,
        });
    REQUIRE(resolved);
    const auto& keybindings = *resolved->registry;

    const auto live = coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::Xterm256);
    CHECK(coding_agent::tui::working_status_message("Working...") == "Working...");
    CHECK(
        coding_agent::tui::retry_status_message(keybindings, 1, 3, 2) ==
        "Retrying (1/3) in 2s... (escape to cancel)");
    CHECK(
        coding_agent::tui::compaction_status_message(keybindings, "manual") ==
        "Compacting context... (escape to cancel)");
    CHECK(
        coding_agent::tui::compaction_status_message(keybindings, "threshold") ==
        "Auto-compacting... (escape to cancel)");
    CHECK(
        coding_agent::tui::compaction_status_message(keybindings, "overflow") ==
        "Context overflow detected, Auto-compacting... (escape to cancel)");
    (void)live;
}

TEST_CASE("StatusIndicator renders the loader row with the message", "[coding_agent][tui][footer][issue411]") {
    const auto live = coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::Xterm256);
    std::size_t renders = 0;
    coding_agent::tui::StatusIndicator indicator(
        coding_agent::tui::StatusIndicator::Kind::Working,
        live,
        [&renders] { ++renders; },
        "Working...");
    auto rendered = indicator.render(40);
    REQUIRE(rendered);
    // pi Loader: one empty spacer row plus the spinner-and-message row.
    REQUIRE(rendered->lines.size() == 2);
    CHECK(rendered->lines[0].empty());
    CHECK(tui::strip_terminal_sequences(rendered->lines[1]).find("Working...") != std::string::npos);

    indicator.set_message("Updated");
    rendered = indicator.render(40);
    REQUIRE(rendered);
    CHECK(tui::strip_terminal_sequences(rendered->lines[1]).find("Updated") != std::string::npos);
    CHECK(renders > 0);

    coding_agent::tui::IdleStatus idle;
    auto idle_rendered = idle.render(40);
    REQUIRE(idle_rendered);
    REQUIRE(idle_rendered->lines.size() == 2);
    CHECK(idle_rendered->lines[0] == std::string(40, ' '));
    CHECK(idle_rendered->lines[1] == std::string(40, ' '));
}
