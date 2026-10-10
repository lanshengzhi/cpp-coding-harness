// Completion contexts for the pi-v1.0.4 baseline (#963). Every provider and
// Editor expectation here is an independent frozen observation replayed through
// `scripts/tui/evidence.py` (fixtures/pi-tui/bundles/pi-v1.0.4/autocomplete.json
// and editor-autocomplete.json), not a value restated from the C++ rules.

#include <cch/tui/Autocomplete.hpp>
#include <cch/tui/Editor.hpp>

#include "tui/UnicodeWidth.hpp"

#include "support/Json.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

constexpr std::string_view kCompletionArtifact = "autocomplete.json";
constexpr std::string_view kEditorTriggerArtifact = "editor-autocomplete.json";

/// The buffer a frozen completion case ran against.
struct RecordedInput {
    std::string base;
    std::string line;
    std::size_t cursor_column{0}; // UTF-16 code units, as pi records it
    bool force{false};
};

/// The prefix, values, and applied text the frozen run observed.
struct RecordedCase {
    std::string name;
    std::optional<std::string> prefix;
    std::optional<std::vector<std::string>> values;
    std::optional<std::vector<std::string>> applied_lines;
    std::optional<std::size_t> applied_cursor_column; // UTF-16 code units
};

struct RecordedProbe {
    std::string character;
    std::string kind;
};

struct RecordedEditorCase {
    std::string name;
    std::string input;
    std::string text;
    std::vector<std::pair<bool, std::string>> requests; // (forced, text before cursor)
};

[[nodiscard]] std::optional<std::string> optional_string(const support::JsonValue& value) {
    if (!value.holds<std::string>()) return std::nullopt;
    return value.get_string();
}

[[nodiscard]] std::optional<std::vector<std::string>> optional_string_array(const support::JsonValue& value) {
    if (!value.holds<support::JsonValue::array_t>()) return std::nullopt;
    std::vector<std::string> result;
    for (const auto& entry : value.get_array()) {
        result.push_back(entry.get_string());
    }
    return result;
}

[[nodiscard]] RecordedInput recorded_input(const support::JsonValue& value) {
    const auto& row = value.get_object();
    return {
            .base = row.at("base").get_string(),
            .line = row.at("line").get_string(),
            .cursor_column = static_cast<std::size_t>(row.at("cursorCol").get_number()),
            .force = row.at("force").get_boolean(),
    };
}

[[nodiscard]] RecordedCase recorded_case(const support::JsonValue& value) {
    const auto& row = value.get_object();
    RecordedCase result{
            .name = row.at("name").get_string(),
            .prefix = optional_string(row.at("prefix")),
            .values = optional_string_array(row.at("values")),
            .applied_lines = optional_string_array(row.at("appliedLines")),
            .applied_cursor_column = std::nullopt,
    };
    if (result.applied_lines) {
        result.applied_cursor_column = static_cast<std::size_t>(row.at("appliedCursorCol").get_number());
    }
    return result;
}

[[nodiscard]] RecordedProbe recorded_probe(const support::JsonValue& value) {
    const auto& row = value.get_object();
    return {.character = row.at("character").get_string(), .kind = row.at("kind").get_string()};
}

[[nodiscard]] RecordedEditorCase recorded_editor_case(const support::JsonValue& value) {
    const auto& row = value.get_object();
    RecordedEditorCase result{
            .name = row.at("name").get_string(),
            .input = row.at("input").get_string(),
            .text = row.at("text").get_string(),
            .requests = {},
    };
    for (const auto& request : row.at("requests").get_array()) {
        const auto& fields = request.get_object();
        result.requests.emplace_back(fields.at("force").get_boolean(), fields.at("text").get_string());
    }
    return result;
}

/// The fixture tree the frozen run built, mirrored so the replay completes the
/// same directory entries (see fixtures/pi-tui/capture/capture-named-tui.mts).
void build_fixtures(const tests::TempWorkspace& workspace, const std::vector<RecordedProbe>& probes) {
    for (const auto& probe : probes) {
        workspace.write("boundary/" + probe.character + "说明.md", "probe");
    }
    workspace.write("boundary/说明.md", "boundary");
    workspace.write("cjk-path/中文/文档.txt", "text");
    workspace.write("cjk-path/文档/说明.md", "text");
    workspace.write("empty-prefix/说明.md", "text");
    workspace.write("quoted/my folder/main.ts", "text");
    workspace.write("quoted/资料，归档/说明.md", "text");
    workspace.write("wrapped/src/main.cc", "text");
    workspace.write("wrapped/(group)/layout.cc", "text");
    workspace.write("wrapped/[slug]/page.tsx", "text");
}

[[nodiscard]] std::vector<std::variant<tui::SlashCommand, tui::AutocompleteItem>> frozen_commands() {
    std::vector<std::variant<tui::SlashCommand, tui::AutocompleteItem>> commands;
    commands.emplace_back(tui::SlashCommand{
            .name = "model",
            .description = "Switch model",
            .get_argument_completions =
                    [](std::string_view argument_prefix) -> std::optional<std::vector<tui::AutocompleteItem>> {
                if (!argument_prefix.starts_with('g')) return std::vector<tui::AutocompleteItem>{};
                return std::vector<tui::AutocompleteItem>{{.value = "gpt", .label = "gpt", .description = {}}};
            },
    });
    commands.emplace_back(tui::SlashCommand{.name = "settings", .description = "Open settings"});
    return commands;
}

[[nodiscard]] std::optional<tui::AutocompleteSuggestions> request_suggestions(
        tui::CombinedAutocompleteProvider& provider, const std::string& line, std::size_t cursor_column, bool force) {
    std::optional<tui::AutocompleteSuggestions> result;
    std::atomic<bool> delivered{false};
    provider.get_suggestions(
            tui::AutocompleteRequest{
                    .lines = {line},
                    .cursor_line = 0,
                    .cursor_column = cursor_column,
                    .force = force,
                    .stop_token = std::stop_source{}.get_token(),
            },
            [&result, &delivered](std::optional<tui::AutocompleteSuggestions> suggestions) -> support::ExpectedVoid {
                result = std::move(suggestions);
                delivered = true;
                return {};
            });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!delivered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return result;
}

/// pi records cursor columns in UTF-16 code units; the C++ completion contract
/// is a UTF-8 byte offset. Convert the frozen column instead of weakening the
/// comparison.
[[nodiscard]] std::optional<std::size_t> byte_offset_of_utf16_column(std::string_view utf8, std::size_t utf16_column) {
    std::size_t units = 0;
    std::size_t index = 0;
    while (index < utf8.size()) {
        if (units == utf16_column) return index;
        const auto lead = static_cast<unsigned char>(utf8[index]);
        const std::size_t length = lead < 0x80 ? 1U : (lead & 0xE0) == 0xC0 ? 2U : (lead & 0xF0) == 0xE0 ? 3U : 4U;
        units += length == 4 ? 2 : 1;
        index += length;
    }
    if (units == utf16_column) return index;
    return std::nullopt;
}

/// Deterministic debounce double: the recorded cases observe request counts, so
/// the queued callback is fired explicitly instead of on a timer.
class ManualDebounceTimer final : public tui::AutocompleteDebounceTimer {
public:
    void start(std::chrono::milliseconds, std::move_only_function<support::ExpectedVoid()> on_fire) override {
        callback = std::move(on_fire);
    }
    void cancel() override { callback = nullptr; }
    void fire() {
        auto on_fire = std::move(callback);
        callback = nullptr;
        if (on_fire) (void)on_fire();
    }

private:
    std::move_only_function<support::ExpectedVoid()> callback;
};

/// Records what the real Editor asked the provider for.
class RecordingProvider final : public tui::AutocompleteProvider {
public:
    void get_suggestions(const tui::AutocompleteRequest& request, tui::AutocompleteResultSink sink) override {
        const auto& line = request.lines[request.cursor_line];
        requests.emplace_back(request.force, line.substr(0, request.cursor_column));
        (void)sink(std::nullopt);
    }

    tui::AutocompleteApplyResult apply_completion(const std::vector<std::string>& lines,
            std::size_t cursor_line,
            std::size_t cursor_column,
            const tui::AutocompleteItem& item,
            std::string_view prefix) override {
        auto result = lines;
        auto& line = result[cursor_line];
        const auto before = line.substr(0, cursor_column - prefix.size());
        line = before + item.value + line.substr(cursor_column);
        return {
                .lines = std::move(result),
                .cursor_line = cursor_line,
                .cursor_column = before.size() + item.value.size(),
        };
    }

    bool should_trigger_file_completion(const std::vector<std::string>&, std::size_t, std::size_t) const override {
        return true;
    }

    std::vector<std::string> trigger_characters() const override { return {}; }

    std::vector<std::pair<bool, std::string>> requests;
};

void send_text(tui::Editor& editor, std::string_view text) {
    static_cast<void>(editor.handle_input(tui::KeyEvent{.key = std::string{text}, .text = std::string{text}}));
}

} // namespace

TEST_CASE("CombinedAutocompleteProvider replays the frozen pi completion contexts",
        "[tui][autocomplete][issue963][compat-pi]") {
    const auto artifact = tests::read_pi_tui_evidence(kCompletionArtifact);
    REQUIRE(artifact);
    const auto& scenario = artifact->at("scenarios").get_array().at(0);
    const auto& inputs = scenario.at("inputs").get_object();
    const auto& expected = scenario.at("expected").get_object();

    std::vector<RecordedProbe> probes;
    for (const auto& probe : inputs.at("classification").get_array()) {
        probes.push_back(recorded_probe(probe));
    }
    REQUIRE_FALSE(probes.empty());

    std::map<std::string, RecordedInput> recorded_inputs;
    for (const auto& entry : inputs.at("cases").get_array()) {
        const auto& row = entry.get_object();
        recorded_inputs.emplace(row.at("name").get_string(), recorded_input(entry));
    }

    tests::TempWorkspace workspace;
    build_fixtures(workspace, probes);

    const auto& recorded = expected.at("cases").get_array();
    REQUIRE_FALSE(recorded.empty());

    for (const auto& entry : recorded) {
        const auto test_case = recorded_case(entry);
        INFO("case " << test_case.name);
        if (test_case.name == "quoted-directory-continuation") {
            const auto cont_input = recorded_input(inputs.at("continuation"));
            const auto cursor_col = byte_offset_of_utf16_column(cont_input.line, cont_input.cursor_column);
            REQUIRE(cursor_col.has_value());
            auto provider = tui::CombinedAutocompleteProvider(
                    frozen_commands(), workspace.path() / cont_input.base, std::nullopt);
            const auto first = request_suggestions(provider, cont_input.line, *cursor_col, cont_input.force);
            REQUIRE(first.has_value());
            const auto applied =
                    provider.apply_completion({cont_input.line}, 0, *cursor_col, first->items.front(), first->prefix);
            const auto continued = request_suggestions(provider, applied.lines.at(0), applied.cursor_column, true);
            REQUIRE(continued.has_value());
            CHECK(continued->prefix == *test_case.prefix);
            REQUIRE(test_case.values);
            std::vector<std::string> values;
            for (const auto& item : continued->items)
                values.push_back(item.value);
            CHECK(values == *test_case.values);
            CHECK(applied.lines == *test_case.applied_lines);
            REQUIRE(test_case.applied_cursor_column);
            const auto applied_col = byte_offset_of_utf16_column(applied.lines.at(0), *test_case.applied_cursor_column);
            REQUIRE(applied_col.has_value());
            CHECK(applied.cursor_column == *applied_col);
            continue;
        }

        const auto input = recorded_inputs.find(test_case.name);
        REQUIRE(input != recorded_inputs.end());
        const auto cursor_column = byte_offset_of_utf16_column(input->second.line, input->second.cursor_column);
        REQUIRE(cursor_column.has_value());

        auto provider = tui::CombinedAutocompleteProvider(
                frozen_commands(), workspace.path() / input->second.base, std::nullopt);
        const auto suggestions = request_suggestions(provider, input->second.line, *cursor_column, input->second.force);

        if (!test_case.prefix) {
            REQUIRE_FALSE(suggestions.has_value());
            continue;
        }
        REQUIRE(suggestions.has_value());
        CHECK(suggestions->prefix == *test_case.prefix);
        REQUIRE(test_case.values);
        std::vector<std::string> values;
        for (const auto& item : suggestions->items)
            values.push_back(item.value);
        CHECK(values == *test_case.values);

        REQUIRE(test_case.applied_lines);
        const auto applied = provider.apply_completion(
                {input->second.line}, 0, *cursor_column, suggestions->items.front(), suggestions->prefix);
        CHECK(applied.lines == *test_case.applied_lines);
        REQUIRE(test_case.applied_cursor_column);
        const auto applied_column = byte_offset_of_utf16_column(applied.lines.at(0), *test_case.applied_cursor_column);
        REQUIRE(applied_column.has_value());
        CHECK(applied.cursor_column == *applied_column);
    }
}

TEST_CASE("Editor completion triggers replay the frozen pi trigger contexts",
        "[tui][editor][autocomplete][issue963][compat-pi]") {
    const auto artifact = tests::read_pi_tui_evidence(kEditorTriggerArtifact);
    REQUIRE(artifact);
    const auto& scenario = artifact->at("scenarios").get_array().at(0);
    const auto& recorded = scenario.at("expected").get_object().at("cases").get_array();
    REQUIRE_FALSE(recorded.empty());

    for (const auto& entry : recorded) {
        const auto test_case = recorded_editor_case(entry);
        INFO("case " << test_case.name);
        auto timer = std::make_unique<ManualDebounceTimer>();
        auto* timer_ptr = timer.get();
        auto provider = std::make_unique<RecordingProvider>();
        auto* provider_ptr = provider.get();
        tui::Editor editor({.autocomplete_debounce_timer = std::move(timer)});
        editor.set_autocomplete_provider(std::move(provider));

        std::size_t offset = 0;
        while (offset < test_case.input.size()) {
            if (test_case.input[offset] == '\t') {
                static_cast<void>(editor.handle_input(tui::KeyEvent{.key = "tab", .text = ""}));
                offset += 1;
                continue;
            }
            const auto [codepoint, length] = cch::tui::detail::decode_utf8(test_case.input, offset);
            send_text(editor, test_case.input.substr(offset, length));
            offset += length;
        }
        timer_ptr->fire();

        CHECK(editor.text() == test_case.text);
        // The frozen run records pi's request list; pi coalesces superseded
        // asynchronous requests, while the C++ Editor asks the provider on its
        // own serialized domain for every triggering keystroke. Coalescing and
        // stale-result rejection belong to the completion lifecycle ticket, so
        // this slice compares the trigger decision (asked at all) and the last
        // request each side made.
        CHECK(provider_ptr->requests.empty() == test_case.requests.empty());
        if (!test_case.requests.empty()) {
            REQUIRE(!provider_ptr->requests.empty());
            CHECK(provider_ptr->requests.back() == test_case.requests.back());
        }
    }
}

TEST_CASE("CombinedAutocompleteProvider completes CJK paths without listing the base directory",
        "[tui][autocomplete][issue963][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("中文/文档.txt", "text");
    workspace.write("notes.md", "other");
    auto provider = tui::CombinedAutocompleteProvider({}, workspace.path(), std::nullopt);

    // Both the partial file name and the trailing directory separator resolve
    // inside ./中文; the base directory listing is never offered.
    for (const auto& line : {std::string{"./中文/文"}, std::string{"./中文/"}}) {
        INFO("line " << line);
        const auto suggestions = request_suggestions(provider, line, line.size(), true);
        REQUIRE(suggestions);
        CHECK(suggestions->prefix == line);
        REQUIRE(suggestions->items.size() == 1);
        CHECK(suggestions->items.front().value == "./中文/文档.txt");
        const auto applied =
                provider.apply_completion({line}, 0, line.size(), suggestions->items.front(), suggestions->prefix);
        CHECK(applied.lines.at(0) == "./中文/文档.txt");
    }

    // A natural (unforced) trigger keeps the CJK directory context as well.
    const std::string natural = "./中文/";
    const auto suggestions = request_suggestions(provider, natural, natural.size(), false);
    REQUIRE(suggestions);
    CHECK(suggestions->items.front().value == "./中文/文档.txt");
}

TEST_CASE("CombinedAutocompleteProvider quotes completion values containing separators",
        "[tui][autocomplete][issue963][spec]") {
    tests::TempWorkspace workspace;
    workspace.write("资料，归档/说明.md", "text");
    auto provider = tui::CombinedAutocompleteProvider({}, workspace.path(), std::nullopt);

    // A CJK punctuation character separates tokens, so a directory containing
    // one is completed as a quoted token and stays completable afterwards.
    const std::string line = "资料";
    const auto suggestions = request_suggestions(provider, line, line.size(), true);
    REQUIRE(suggestions);
    REQUIRE(suggestions->items.size() == 1);
    CHECK(suggestions->items.front().value == "\"资料，归档/\"");

    const auto applied =
            provider.apply_completion({line}, 0, line.size(), suggestions->items.front(), suggestions->prefix);
    CHECK(applied.lines.at(0) == "\"资料，归档/\"");
    const auto continued = request_suggestions(provider, applied.lines.at(0), applied.cursor_column, true);
    REQUIRE(continued);
    CHECK(continued->prefix == "\"资料，归档/");
    REQUIRE(continued->items.size() == 1);
    CHECK(continued->items.front().value == "\"资料，归档/说明.md\"");
}
