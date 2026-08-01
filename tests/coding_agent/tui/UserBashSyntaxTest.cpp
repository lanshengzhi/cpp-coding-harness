#include "coding_agent/tui/UserBashSyntax.hpp"

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <optional>
#include <string>

using cch::coding_agent::tui::parse_user_bash_invocation;
using cch::coding_agent::tui::safe_user_bash_invocation;
using cch::coding_agent::tui::trim_editor_submission;
using cch::coding_agent::tui::user_bash_editor_mode;
using cch::coding_agent::tui::UserBashInvocation;

TEST_CASE(
    "trim_editor_submission strips ASCII whitespace from both ends only",
    "[coding_agent][tui][user-bash-syntax]") {
    CHECK(trim_editor_submission("  ls -la\t\n") == "ls -la");
    CHECK(trim_editor_submission("no-op") == "no-op");
    CHECK(trim_editor_submission(" \t\r\n ").empty());
    CHECK(trim_editor_submission("").empty());
    CHECK(trim_editor_submission("a b") == "a b");
}

TEST_CASE(
    "parse_user_bash_invocation interprets the baseline prefix table",
    "[coding_agent][tui][user-bash-syntax]") {
    struct Case {
        std::string input;
        std::optional<UserBashInvocation> expected;
    };
    const Case cases[] = {
        {"!ls", UserBashInvocation{.command = "ls", .exclude_from_context = false}},
        {"!!ls", UserBashInvocation{.command = "ls", .exclude_from_context = true}},
        {"  ! ls -la  ", UserBashInvocation{.command = "ls -la", .exclude_from_context = false}},
        // `!!!foo` is excluded User Bash running `!foo`.
        {"!!!foo", UserBashInvocation{.command = "!foo", .exclude_from_context = true}},
        // Bare prefixes fall through to an ordinary Agent Prompt.
        {"!", std::nullopt},
        {"!!", std::nullopt},
        {"!   ", std::nullopt},
        {"!!\t", std::nullopt},
        // Ordinary prompts are not User Bash.
        {"ls", std::nullopt},
        {"hello !world", std::nullopt},
        {"", std::nullopt},
        // Commands may span multiple lines; inner whitespace is preserved.
        {"!echo a\nb", UserBashInvocation{.command = "echo a\nb", .exclude_from_context = false}},
    };

    for (const auto& test : cases) {
        auto parsed = parse_user_bash_invocation(test.input);
        if (!test.expected) {
            CHECK_FALSE(parsed.has_value());
            continue;
        }
        REQUIRE(parsed.has_value());
        CHECK(parsed->command == test.expected->command);
        CHECK(parsed->exclude_from_context == test.expected->exclude_from_context);
    }
}

TEST_CASE(
    "safe_user_bash_invocation restores the prefix and strips unsafe bytes",
    "[coding_agent][tui][user-bash-syntax]") {
    CHECK(safe_user_bash_invocation(
              UserBashInvocation{.command = "ls", .exclude_from_context = false}) ==
        "! ls");
    CHECK(safe_user_bash_invocation(
              UserBashInvocation{.command = "ls", .exclude_from_context = true}) ==
        "!! ls");
    // Terminal escape sequences and control bytes never reach the editor or
    // diagnostics.
    CHECK(safe_user_bash_invocation(UserBashInvocation{
              .command = std::string{"e\x1b[31mcho\x07 hi"},
              .exclude_from_context = false,
          }) == "! echo hi");
}

TEST_CASE(
    "user_bash_editor_mode requires availability and a trimmed bang prefix",
    "[coding_agent][tui][user-bash-syntax]") {
    CHECK(user_bash_editor_mode("!ls", true));
    CHECK(user_bash_editor_mode("  !ls", true));
    // A lone `!` is already Bash mode while unsubmitted, matching the
    // editor's live theming even though submission falls through.
    CHECK(user_bash_editor_mode("!", true));
    CHECK_FALSE(user_bash_editor_mode("ls", true));
    CHECK_FALSE(user_bash_editor_mode("", true));
    CHECK_FALSE(user_bash_editor_mode("!ls", false));
}
