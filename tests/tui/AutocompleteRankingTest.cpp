#include <cch/tui/Autocomplete.hpp>

#include "support/TempWorkspace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using cch::tui::AutocompleteItem;
using cch::tui::AutocompleteProvider;
using cch::tui::AutocompleteRequest;
using cch::tui::AutocompleteResultSink;
using cch::tui::AutocompleteSuggestions;
using cch::tui::CombinedAutocompleteProvider;
using cch::tui::SlashCommand;

// Frozen ranking evidence: every expected list below was observed by running
// the frozen pi v1.0.4 packages/tui/src/autocomplete.ts at
// 7c10bd4337495ee613f2224843ecdf349b80d1df directly (Node v26.11.1, Combined-
// AutocompleteProvider against the same command lists, fake-fd fixtures and
// workspace layouts used here). This records the independent pi comparison
// required by #964 without regenerating the named evidence bundle.

[[nodiscard]] std::optional<AutocompleteSuggestions> request_suggestions(AutocompleteProvider& provider,
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
    provider.get_suggestions(request,
            [&result, &delivered](std::optional<AutocompleteSuggestions> suggestions) -> cch::support::ExpectedVoid {
                result = std::move(suggestions);
                delivered = true;
                return {};
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

[[nodiscard]] std::vector<std::string> values_of(const AutocompleteSuggestions& suggestions) {
    std::vector<std::string> values;
    values.reserve(suggestions.items.size());
    for (const auto& item : suggestions.items)
        values.push_back(item.value);
    return values;
}

/// Write an executable fake `fd` script printing `lines` verbatim (one entry
/// per line), ignoring its arguments.
[[nodiscard]] std::filesystem::path write_fake_fd(
        cch::tests::TempWorkspace& workspace, std::string_view name, const std::vector<std::string>& lines) {
    std::string script = "#!/bin/sh\n";
    for (const auto& line : lines) {
        script += "printf '%s\\n' '" + line + "'\n";
    }
    script += "exit 0\n";
    const auto relative = std::filesystem::path{"fake-fd"} / name;
    workspace.write(relative.string(), script);
    const auto path = workspace.path() / relative;
    std::filesystem::permissions(path,
            std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                    std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    return path;
}

} // namespace

// pi's own regression fixture (packages/tui/test/autocomplete-skill-slash.test.ts
// at the frozen commit) run through the observed frozen provider.
TEST_CASE("CombinedAutocompleteProvider ranks skill commands by bare name ahead of full-name matches",
        "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;
    std::vector<std::variant<SlashCommand, AutocompleteItem>> commands;
    commands.push_back(AutocompleteItem{.value = "skill:deep-research",
            .label = "skill:deep-research",
            .description = "Multi-agent deep research"});
    commands.push_back(AutocompleteItem{.value = "skill:research-idea",
            .label = "skill:research-idea",
            .description = "Refine a raw idea into a falsifiable seed"});
    commands.push_back(AutocompleteItem{
            .value = "skill:to-sidecar", .label = "skill:to-sidecar", .description = "Route work to a sidecar"});
    commands.push_back(AutocompleteItem{
            .value = "skill:brainstorm", .label = "skill:brainstorm", .description = "Generate ideas"});
    commands.push_back(AutocompleteItem{.value = "model", .label = "model", .description = "Select the active model"});
    auto provider = make_provider(std::move(commands), workspace.path());

    // Bare-name matches rank ahead of `skill:` full-name-only matches, even
    // where the full-name match would score better under the replaced
    // uniform full-name filtering. Frozen pi: ["skill:research-idea",
    // "skill:to-sidecar", "skill:deep-research"].
    const auto idea = request_suggestions(provider, {"/idea"}, 0, 5);
    REQUIRE(idea);
    CHECK(values_of(*idea) ==
            std::vector<std::string>{"skill:research-idea", "skill:to-sidecar", "skill:deep-research"});
    CHECK(idea->items[0].description == "Refine a raw idea into a falsifiable seed");

    // Typing the skill prefix still lists every skill in declaration order
    // (pi #9944): the bare pass matches nothing, the full-name pass keeps
    // all four with equal scores.
    const auto skill = request_suggestions(provider, {"/skill"}, 0, 6);
    REQUIRE(skill);
    CHECK(values_of(*skill) ==
            std::vector<std::string>{
                    "skill:deep-research", "skill:research-idea", "skill:to-sidecar", "skill:brainstorm"});

    // A fuzzy shorthand that only the full name reaches still works.
    const auto shorthand = request_suggestions(provider, {"/skbra"}, 0, 6);
    REQUIRE(shorthand);
    CHECK(values_of(*shorthand) == std::vector<std::string>{"skill:brainstorm"});

    // An explicit `skill:` query matches full names only.
    const auto explicit_skill = request_suggestions(provider, {"/skill:side"}, 0, 11);
    REQUIRE(explicit_skill);
    CHECK(values_of(*explicit_skill) == std::vector<std::string>{"skill:to-sidecar", "skill:research-idea"});

    // Ordinary commands still match by their (unprefixed) name.
    const auto model = request_suggestions(provider, {"/mod"}, 0, 4);
    REQUIRE(model);
    CHECK(values_of(*model) == std::vector<std::string>{"model"});
}

TEST_CASE("CombinedAutocompleteProvider ranks the bare skill name review ahead of other matches",
        "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;
    std::vector<std::variant<SlashCommand, AutocompleteItem>> commands;
    commands.push_back(
            AutocompleteItem{.value = "skill:review", .label = "skill:review", .description = "Review a change"});
    commands.push_back(AutocompleteItem{.value = "skill:deep-research",
            .label = "skill:deep-research",
            .description = "Multi-agent deep research"});
    commands.push_back(AutocompleteItem{.value = "review-board", .label = "review-board", .description = "Board"});
    commands.push_back(AutocompleteItem{.value = "model", .label = "model", .description = {}});
    auto provider = make_provider(std::move(commands), workspace.path());

    // Frozen pi: ["skill:review", "review-board"]. The replaced uniform
    // full-name policy ranked "review-board" first (word-boundary bonus on
    // the unprefixed name beats the mid-name match inside "skill:review"),
    // so this order separates the two-pass policy from the replaced one.
    const auto review = request_suggestions(provider, {"/review"}, 0, 7);
    REQUIRE(review);
    CHECK(values_of(*review) == std::vector<std::string>{"skill:review", "review-board"});
    CHECK(review->items[0].description == "Review a change");
    // "skill:deep-research" matches neither the bare nor the full name for
    // this query and stays out of the list.
}

TEST_CASE("CombinedAutocompleteProvider keeps slash argument callables working for skill commands",
        "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;
    std::vector<std::variant<SlashCommand, AutocompleteItem>> commands;
    commands.push_back(SlashCommand{
            .name = "skill:review",
            .description = "Review a change",
            .argument_hint = {},
            .get_argument_completions =
                    [](std::string_view argument_prefix) -> std::optional<std::vector<AutocompleteItem>> {
                return std::vector<AutocompleteItem>{
                        {.value = "arg:" + std::string{argument_prefix}, .label = "arg", .description = {}}};
            },
    });
    commands.push_back(AutocompleteItem{.value = "model", .label = "model", .description = {}});
    auto provider = make_provider(std::move(commands), workspace.path());

    // Frozen pi: "/skill:review diff" calls the command's
    // getArgumentCompletions("diff") and prefixes with "diff".
    const std::string line = "/skill:review diff";
    const auto arguments = request_suggestions(provider, {line}, 0, line.size());
    REQUIRE(arguments);
    CHECK(arguments->prefix == "diff");
    REQUIRE(arguments->items.size() == 1);
    CHECK(arguments->items[0].value == "arg:diff");

    // The command name itself still completes through the two-pass ranking.
    const auto name = request_suggestions(provider, {"/skill:rev"}, 0, 10);
    REQUIRE(name);
    CHECK(values_of(*name) == std::vector<std::string>{"skill:review"});
}

// The tie-break fixture: nine candidates, one per ranking level. Frozen pi
// (both fd orders) returns "@aa/", "@aa2/", "@bb/aa.cc", "@zz/zaa.cc",
// "@aa/dir/", "@aa/x0.cc", "@aa/x1.cc", "@aa/x3long.cc", "@aa/sub/y2.cc":
// score first (exact directory name 110, directory prefix 90, file prefix 80,
// name substring 50, directory path substring 40), then depth, path length
// and name for the equal-score file group.
TEST_CASE("CombinedAutocompleteProvider orders equal-score @ matches by depth, length and name",
        "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;
    const std::vector<std::string> entries{
            "aa/sub/y2.cc",
            "aa/x3long.cc",
            "aa/x1.cc",
            "aa/x0.cc",
            "aa/dir/",
            "zz/zaa.cc",
            "bb/aa.cc",
            "aa2/",
            "aa/",
    };
    std::vector<std::string> reversed(entries.rbegin(), entries.rend());

    const std::vector<std::string> expected_values{
            "@aa/",
            "@aa2/",
            "@bb/aa.cc",
            "@zz/zaa.cc",
            "@aa/dir/",
            "@aa/x0.cc",
            "@aa/x1.cc",
            "@aa/x3long.cc",
            "@aa/sub/y2.cc",
    };
    const std::vector<std::string> expected_labels{
            "aa/",
            "aa2/",
            "aa.cc",
            "zaa.cc",
            "dir/",
            "x0.cc",
            "x1.cc",
            "x3long.cc",
            "y2.cc",
    };
    const std::vector<std::string> expected_descriptions{
            "aa",
            "aa2",
            "bb/aa.cc",
            "zz/zaa.cc",
            "aa/dir",
            "aa/x0.cc",
            "aa/x1.cc",
            "aa/x3long.cc",
            "aa/sub/y2.cc",
    };

    // The ordered list is independent of fd's return order: a scrambled and a
    // reversed fake fd produce the same frozen result.
    const std::array<const std::vector<std::string>*, 2> fixtures{&entries, &reversed};
    for (const auto* fixture : fixtures) {
        const auto fake_fd = write_fake_fd(workspace, fixture == &entries ? "fd-a" : "fd-b", *fixture);
        auto provider = make_provider({}, workspace.path(), fake_fd);
        const auto suggestions = request_suggestions(provider, {"@aa"}, 0, 3);
        REQUIRE(suggestions);
        CHECK(suggestions->prefix == "@aa");
        CHECK(values_of(*suggestions) == expected_values);
        std::vector<std::string> labels;
        std::vector<std::string> descriptions;
        for (const auto& item : suggestions->items) {
            labels.push_back(item.label);
            descriptions.push_back(item.description);
        }
        CHECK(labels == expected_labels);
        CHECK(descriptions == expected_descriptions);
    }
}

TEST_CASE("CombinedAutocompleteProvider truncates the ordered @ list at twenty entries",
        "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;
    // 25 equal-score candidates in scrambled order; the frozen truncation
    // keeps the first twenty after the depth/length/name tie-breaks, so the
    // cut lands at f19 regardless of fd's return order.
    std::vector<std::string> entries;
    for (int index = 19; index >= 0; --index) {
        entries.push_back("aa/f" + std::string{static_cast<char>('0' + index / 10)} +
                          static_cast<char>('0' + index % 10) + ".cc");
    }
    for (int index = 24; index >= 20; --index) {
        entries.push_back("aa/f" + std::string{static_cast<char>('0' + index / 10)} +
                          static_cast<char>('0' + index % 10) + ".cc");
    }
    std::vector<std::string> reversed(entries.rbegin(), entries.rend());

    std::vector<std::string> expected;
    for (int index = 0; index < 20; ++index) {
        expected.push_back("@aa/f" + std::string{static_cast<char>('0' + index / 10)} +
                           static_cast<char>('0' + index % 10) + ".cc");
    }

    const std::array<const std::vector<std::string>*, 2> fixtures{&entries, &reversed};
    for (const auto* fixture : fixtures) {
        const auto fake_fd = write_fake_fd(workspace, fixture == &entries ? "fd-a" : "fd-b", *fixture);
        auto provider = make_provider({}, workspace.path(), fake_fd);
        const auto suggestions = request_suggestions(provider, {"@aa"}, 0, 3);
        REQUIRE(suggestions);
        CHECK(values_of(*suggestions) == expected);
    }
}

TEST_CASE("CombinedAutocompleteProvider orders an empty @ query purely by the tie-breaks",
        "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;
    // Empty query: every entry scores 1 in frozen pi, so depth, path length
    // and name alone decide. Frozen pi: "@bb/", "@a.cc", "@aa.cc", "@bb/cc/",
    // "@bb/b.cc", "@bb/cc/dd.cc".
    const auto fake_fd = write_fake_fd(workspace, "fd", {"bb/cc/dd.cc", "a.cc", "bb/cc/", "bb/", "aa.cc", "bb/b.cc"});
    auto provider = make_provider({}, workspace.path(), fake_fd);

    const auto suggestions = request_suggestions(provider, {"@"}, 0, 1);
    REQUIRE(suggestions);
    CHECK(values_of(*suggestions) ==
            std::vector<std::string>{"@bb/", "@a.cc", "@aa.cc", "@bb/cc/", "@bb/b.cc", "@bb/cc/dd.cc"});
}

TEST_CASE("CombinedAutocompleteProvider keeps scoped direct children when recursive @ matches flood the window",
        "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;
    workspace.write("scope/placeholder.txt", "\n");
    // Fake fd emulating the frozen windows for query "pro": the depth-1
    // (base-directory) walk finds only "projects/"; the recursive walk's
    // first 100 results are all deep "profile/" directories and drop
    // "projects/". Frozen pi (its own flood regression fixture at #8669)
    // keeps "@scope/projects/" first and truncates the deep matches.
    std::string script = R"(#!/bin/sh
max_depth=""
max_results=100
while [ $# -gt 0 ]; do
  case "$1" in
    --max-depth) max_depth="$2"; shift 2 ;;
    --max-results) max_results="$2"; shift 2 ;;
    *) shift ;;
  esac
done
if [ -n "$max_depth" ]; then
  printf '%s\n' 'projects/'
  exit 0
fi
{
)";
    for (int index = 1; index <= 100; ++index) {
        script += "  printf '%s\\n' 'a" + std::string{static_cast<char>('0' + index / 100 % 10)} +
                  static_cast<char>('0' + index / 10 % 10) + static_cast<char>('0' + index % 10) +
                  "/venv/lib/python3.12/site-packages/pkg/core/profile/'\n";
    }
    script += R"(  printf '%s\n' 'projects/'
} | head -n "$max_results"
exit 0
)";
    workspace.write("fake-fd/fd", script);
    const auto fake_fd = workspace.path() / "fake-fd" / "fd";
    std::filesystem::permissions(fake_fd,
            std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
                    std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    auto provider = make_provider({}, workspace.path(), fake_fd);

    const std::string line = "@scope/pro";
    const auto suggestions = request_suggestions(provider, {line}, 0, line.size());
    REQUIRE(suggestions);
    // Twenty items: the rescued direct child plus the first nineteen deep
    // matches in name order (a001..a019), exactly as frozen pi truncates.
    REQUIRE(suggestions->items.size() == 20);
    CHECK(suggestions->items[0].value == "@scope/projects/");
    CHECK(suggestions->items[0].label == "projects/");
    CHECK(suggestions->items[0].description == "scope/projects");
    CHECK(suggestions->items[1].value == "@scope/a001/venv/lib/python3.12/site-packages/pkg/core/profile/");
    CHECK(suggestions->items[19].value == "@scope/a019/venv/lib/python3.12/site-packages/pkg/core/profile/");
    for (std::size_t index = 1; index < suggestions->items.size(); ++index) {
        CHECK(suggestions->items[index].value.find("/profile/") != std::string::npos);
    }
}

TEST_CASE("CombinedAutocompleteProvider degrades gracefully when fd is absent", "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;

    // Frozen pi returns null (no suggestions) both when fdPath is null and
    // when the fd binary cannot be spawned.
    auto without_fd = make_provider({}, workspace.path(), std::nullopt);
    CHECK_FALSE(request_suggestions(without_fd, {"@aa"}, 0, 3).has_value());

    auto missing_binary = make_provider({}, workspace.path(), workspace.path() / "no-such-fd");
    CHECK_FALSE(request_suggestions(missing_binary, {"@aa"}, 0, 3).has_value());
}

TEST_CASE("CombinedAutocompleteProvider sorts quoted directories with directories in readdir completion",
        "[tui][autocomplete][issue964]") {
    cch::tests::TempWorkspace workspace;
    workspace.write("dir with space/.keep", "");
    workspace.write("zzz/.keep", "");
    workspace.write("aaa.cc", "x\n");
    auto provider = make_provider({}, workspace.path());

    // Frozen pi classifies directories by the label's trailing slash, so a
    // quoted directory value ("dir with space/") still sorts with
    // directories, ahead of files. Frozen pi (forced Tab on an empty line):
    // ["\"dir with space/\"", "zzz/", "aaa.cc"].
    const auto forced = request_suggestions(provider, {""}, 0, 0, /*force=*/true);
    REQUIRE(forced);
    CHECK(values_of(*forced) == std::vector<std::string>{"\"dir with space/\"", "zzz/", "aaa.cc"});
    CHECK(forced->items[0].label == "dir with space/");

    // The natural "./" prefix orders identically. Frozen pi:
    // ["\"./dir with space/\"", "./zzz/", "./aaa.cc"].
    const auto natural = request_suggestions(provider, {"./"}, 0, 2);
    REQUIRE(natural);
    CHECK(values_of(*natural) == std::vector<std::string>{"\"./dir with space/\"", "./zzz/", "./aaa.cc"});
}
