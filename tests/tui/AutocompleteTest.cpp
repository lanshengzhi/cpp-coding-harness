#include <cch/tui/Autocomplete.hpp>
#include <cch/tui/Keys.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"
#include "../support/TempWorkspace.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using cch::tui::AutocompleteApplyResult;
using cch::tui::AutocompleteItem;
using cch::tui::AutocompleteProvider;
using cch::tui::AutocompleteRequest;
using cch::tui::AutocompleteResultSink;
using cch::tui::AutocompleteSuggestions;
using cch::tui::CombinedAutocompleteProvider;
using cch::tui::SlashCommand;

[[nodiscard]] std::optional<AutocompleteSuggestions> request_suggestions(
    AutocompleteProvider& provider,
    std::vector<std::string> lines,
    std::size_t cursor_line,
    std::size_t cursor_column,
    bool force = false) {
    std::optional<AutocompleteSuggestions> result;
    std::atomic<bool> delivered{false};
    AutocompleteRequest request{
        .lines = std::move(lines),
        .cursor_line = cursor_line,
        .cursor_column = cursor_column,
        .force = force,
        .stop_token = std::stop_source{}.get_token(),
    };
    provider.get_suggestions(request, [&result, &delivered](std::optional<AutocompleteSuggestions> suggestions) {
        result = std::move(suggestions);
        delivered = true;
    });
    // fd-backed requests deliver from a worker thread.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!delivered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return result;
}

[[nodiscard]] CombinedAutocompleteProvider make_provider(
    std::vector<std::variant<SlashCommand, AutocompleteItem>> commands,
    std::filesystem::path base_path,
    std::optional<std::filesystem::path> fd_path = std::nullopt) {
    return CombinedAutocompleteProvider(std::move(commands), std::move(base_path), std::move(fd_path));
}

/// Write an executable fake `fd` script printing `output` verbatim (one
/// entry per line) and returning `exit_code`.
[[nodiscard]] std::filesystem::path write_fake_fd(
    cch::tests::TempWorkspace& workspace,
    std::string output,
    int exit_code = 0) {
    workspace.write(
        "fake-fd/fd",
        "#!/bin/sh\n"
        "printf '%s\\n' '" +
            output +
            "'\n"
            "exit " +
            std::to_string(exit_code) + "\n");
    const auto path = workspace.path() / "fake-fd" / "fd";
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    return path;
}

} // namespace

TEST_CASE("CombinedAutocompleteProvider completes slash commands with fuzzy ranking", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    std::vector<std::variant<SlashCommand, AutocompleteItem>> commands;
    commands.push_back(SlashCommand{.name = "settings", .description = "Open settings", .argument_hint = "section"});
    commands.push_back(SlashCommand{.name = "hotkeys", .description = "List hotkeys"});
    commands.push_back(AutocompleteItem{.value = "model", .label = "model", .description = "Switch model"});
    commands.push_back(AutocompleteItem{.value = "exit", .label = "exit", .description = {}});
    auto provider = make_provider(std::move(commands), workspace.path());

    // Empty prefix lists every command with pi's hint-merged descriptions.
    const auto all = request_suggestions(provider, {"/"}, 0, 1);
    REQUIRE(all);
    REQUIRE(all->items.size() == 4);
    CHECK(all->prefix == "/");
    CHECK(all->items[0].value == "settings");
    CHECK(all->items[0].description == "section \u2014 Open settings");
    CHECK(all->items[1].description == "List hotkeys");

    // Fuzzy subsequence filtering and ranking (pi fuzzyFilter).
    const auto filtered = request_suggestions(provider, {"/set"}, 0, 4);
    REQUIRE(filtered);
    REQUIRE(filtered->items.size() == 1);
    CHECK(filtered->items[0].value == "settings");
    CHECK(filtered->prefix == "/set");

    // Non-slash text yields no suggestions.
    CHECK_FALSE(request_suggestions(provider, {"hello"}, 0, 5).has_value());

    // A forced request skips the slash branch (pi's !options.force gate).
    CHECK_FALSE(request_suggestions(provider, {"/set"}, 0, 4, /*force=*/true).has_value());
}

TEST_CASE("CombinedAutocompleteProvider completes command arguments through SlashCommand", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    std::vector<std::variant<SlashCommand, AutocompleteItem>> commands;
    commands.push_back(SlashCommand{
        .name = "model",
        .description = {},
        .argument_hint = {},
        .get_argument_completions =
            [](std::string_view argument_prefix) -> std::optional<std::vector<AutocompleteItem>> {
            if (argument_prefix.empty()) {
                return std::vector<AutocompleteItem>{{.value = "gpt", .label = "gpt", .description = {}}};
            }
            if (argument_prefix.starts_with("g")) {
                return std::vector<AutocompleteItem>{{.value = "gpt", .label = "gpt", .description = {}}};
            }
            return std::vector<AutocompleteItem>{};
        },
    });
    commands.push_back(AutocompleteItem{.value = "plain", .label = "plain", .description = {}});
    auto provider = make_provider(std::move(commands), workspace.path());

    const auto arguments = request_suggestions(provider, {"/model "}, 0, 7);
    REQUIRE(arguments);
    REQUIRE(arguments->items.size() == 1);
    CHECK(arguments->items[0].value == "gpt");
    CHECK(arguments->prefix == "");

    const auto partial = request_suggestions(provider, {"/model g"}, 0, 8);
    REQUIRE(partial);
    CHECK(partial->prefix == "g");

    // Commands without an argument-completion callback return no suggestions.
    CHECK_FALSE(request_suggestions(provider, {"/plain "}, 0, 7).has_value());
}

TEST_CASE("CombinedAutocompleteProvider completes paths from the base directory", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    workspace.write("src/main.cc", "int main() {}\n");
    workspace.write("src/util/helper.cc", "// helper\n");
    workspace.write("tests/run.cc", "// run\n");
    workspace.write("README.md", "# readme\n");
    auto provider = make_provider({}, workspace.path());

    // A path-like prefix opens file completion (directories first, then
    // alphabetical labels, case-insensitive prefix filter).
    const auto scoped = request_suggestions(provider, {"src/m"}, 0, 5);
    REQUIRE(scoped);
    REQUIRE(scoped->items.size() == 1);
    CHECK(scoped->items[0].value == "src/main.cc");
    CHECK(scoped->prefix == "src/m");

    // Trailing slash lists directory contents, directories first.
    const auto contents = request_suggestions(provider, {"src/"}, 0, 4);
    REQUIRE(contents);
    REQUIRE(contents->items.size() == 2);
    CHECK(contents->items[0].value == "src/util/");
    CHECK(contents->items[0].label == "util/");
    CHECK(contents->items[1].value == "src/main.cc");
    CHECK(contents->items[1].label == "main.cc");

    // A forced request (Tab) returns the last token even without path shape.
    const auto forced = request_suggestions(provider, {"src"}, 0, 3, /*force=*/true);
    REQUIRE(forced);
    REQUIRE_FALSE(forced->items.empty());

    // Empty text after a space is a natural path context (pi's empty-after-space).
    const auto after_space = request_suggestions(provider, {"hello "}, 0, 6);
    REQUIRE(after_space);
    CHECK_FALSE(after_space->items.empty());

    // A plain token without path shape does not trigger naturally.
    CHECK_FALSE(request_suggestions(provider, {"hello"}, 0, 5).has_value());
}

TEST_CASE("CombinedAutocompleteProvider offers @ attachment completion through fd", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    workspace.write("src/main.cc", "int main() {}\n");
    workspace.write("src/deep/nested.cc", "// nested\n");
    const auto fake_fd = write_fake_fd(
        workspace,
        "main.cc\n"
        "deep/\n"
        "nested.cc\n"
        "README.md\n"
        ".git/HEAD\n"
        "src/.git/config\n");
    auto provider = make_provider({}, workspace.path(), fake_fd);

    // The @ branch resolves the query scope and ranks by filename score.
    const auto suggestions = request_suggestions(provider, {"@src/de"}, 0, 7);
    REQUIRE(suggestions);
    REQUIRE_FALSE(suggestions->items.empty());
    CHECK(suggestions->prefix == "@src/de");
    // Exact filename match scores highest; .git entries are excluded.
    const auto& first = suggestions->items.front();
    CHECK(first.value == "@src/deep/");
    CHECK(first.label == "deep/");
    CHECK(first.description == "src/deep");
    for (const auto& item : suggestions->items) {
        CHECK(item.value.find(".git") == std::string::npos);
    }

    // An @ prefix with an empty fd path degrades gracefully to null (pi's
    // fdPath: null branch).
    auto fdless = make_provider({}, workspace.path(), std::nullopt);
    CHECK_FALSE(request_suggestions(fdless, {"@src"}, 0, 4).has_value());

    // The forced path branch (Tab) works without fd through readdir.
    const auto forced = request_suggestions(provider, {"src"}, 0, 3, /*force=*/true);
    REQUIRE(forced);
    CHECK_FALSE(forced->items.empty());
}

TEST_CASE("CombinedAutocompleteProvider applyCompletion performs pi's text surgery", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    auto provider = make_provider({}, workspace.path());

    // Slash command: "/name " with a trailing space after the name.
    const auto slash = provider.apply_completion(
        {"/set"}, 0, 4, AutocompleteItem{.value = "settings", .label = "settings"}, "/set");
    REQUIRE(slash.lines.size() == 1);
    CHECK(slash.lines[0] == "/settings ");
    CHECK(slash.cursor_column == 10);

    // @ attachment: value carries the @; no trailing space after directories.
    const auto at = provider.apply_completion(
        {"@sr"}, 0, 3, AutocompleteItem{.value = "@src/", .label = "src/"}, "@sr");
    CHECK(at.lines[0] == "@src/");
    CHECK(at.cursor_column == 5);

    // @ attachment file: trailing space after files.
    const auto at_file = provider.apply_completion(
        {"@sr"}, 0, 3, AutocompleteItem{.value = "@src/main.cc", .label = "main.cc"}, "@sr");
    CHECK(at_file.lines[0] == "@src/main.cc ");
    CHECK(at_file.cursor_column == 13);

    // Command argument context: no leading slash re-insertion.
    const auto argument = provider.apply_completion(
        {"/model g"}, 0, 8, AutocompleteItem{.value = "gpt", .label = "gpt"}, "g");
    CHECK(argument.lines[0] == "/model gpt");
    CHECK(argument.cursor_column == 10);

    // Plain path completion.
    const auto path = provider.apply_completion(
        {"src/m"}, 0, 5, AutocompleteItem{.value = "src/main.cc", .label = "main.cc"}, "src/m");
    CHECK(path.lines[0] == "src/main.cc");
    CHECK(path.cursor_column == 11);

    // Quoted @ prefix: the item carries the closing quote, the typed leading
    // quote after the cursor is dropped, and files still get a trailing space.
    const auto quoted = provider.apply_completion(
        {"@\"src/m\"x"}, 0, 7, AutocompleteItem{.value = "@\"src/main.cc\"", .label = "main.cc"}, "@\"src/m");
    CHECK(quoted.lines[0] == "@\"src/main.cc\" x");
    CHECK(quoted.cursor_column == 15);
}

TEST_CASE("CombinedAutocompleteProvider shouldTriggerFileCompletion defers to slash commands", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    auto provider = make_provider({}, workspace.path());

    // A slash command without a space does not trigger file completion.
    CHECK_FALSE(provider.should_trigger_file_completion({"/set"}, 0, 4));
    // Anything else (including slash commands with arguments) does.
    CHECK(provider.should_trigger_file_completion({"/set foo"}, 0, 8));
    CHECK(provider.should_trigger_file_completion({"hello"}, 0, 5));
}

TEST_CASE("CombinedAutocompleteProvider aborts the fd walk on stop request", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    const auto fake_fd = write_fake_fd(
        workspace,
        "main.cc\n"
        "deep/\n");
    auto provider = make_provider({}, workspace.path(), fake_fd);

    // A request whose token is already stopped delivers null immediately and
    // never spawns fd.
    std::stop_source source;
    source.request_stop();
    bool delivered = false;
    provider.get_suggestions(
        AutocompleteRequest{
            .lines = {"@src"},
            .cursor_line = 0,
            .cursor_column = 4,
            .force = false,
            .stop_token = source.get_token(),
        },
        [&delivered](std::optional<AutocompleteSuggestions> suggestions) {
            delivered = true;
            CHECK_FALSE(suggestions.has_value());
        });
    CHECK(delivered);
}

TEST_CASE("CombinedAutocompleteProvider fd walk delivers async results from a worker", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    const auto fake_fd = write_fake_fd(workspace, "main.cc\n");
    auto provider = make_provider({}, workspace.path(), fake_fd);

    // Slow the fake fd down so the sink provably fires after get_suggestions
    // returns, from the worker thread.
    workspace.write(
        "fake-fd/fd",
        "#!/bin/sh\n"
        "sleep 0.05\n"
        "printf '%s\\n' 'main.cc'\n"
        "exit 0\n");
    std::filesystem::permissions(
        workspace.path() / "fake-fd" / "fd",
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);

    bool delivered = false;
    std::optional<AutocompleteSuggestions> result;
    provider.get_suggestions(
        AutocompleteRequest{
            .lines = {"@ma"},
            .cursor_line = 0,
            .cursor_column = 3,
            .force = false,
            .stop_token = std::stop_source{}.get_token(),
        },
        [&](std::optional<AutocompleteSuggestions> suggestions) {
            delivered = true;
            result = std::move(suggestions);
        });
    CHECK_FALSE(delivered);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!delivered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(delivered);
    REQUIRE(result);
    REQUIRE(result->items.size() == 1);
    CHECK(result->items[0].value == "@main.cc");
    CHECK(result->prefix == "@ma");
}

TEST_CASE("CombinedAutocompleteProvider shouldTriggerFileCompletion trims both ends", "[tui][autocomplete][issue383]") {
    cch::tests::TempWorkspace workspace;
    auto provider = make_provider({}, workspace.path());

    // A slash command with a trailing space (JS trim()) still blocks forced
    // file completion.
    CHECK_FALSE(provider.should_trigger_file_completion({"/cmd "}, 0, 5));
    CHECK_FALSE(provider.should_trigger_file_completion({" /cmd"}, 0, 5));
    CHECK(provider.should_trigger_file_completion({" /cmd foo"}, 0, 9));
}
