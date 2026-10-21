// #1044: pi-faithful `/export` HTML (pi v1.3.0 `core/export-html`). The
// exporter vendors pi's template.html/css/js plus the marked/highlight
// bundles and performs pi's exact substitution, so the assertions below
// discriminate the pi contract, not a "simplified HTML":
//
//   * every named template slot is gone from the emitted document,
//   * the injected JavaScript keeps JS `String.prototype.replace` semantics
//     (`$$` collapses to `$`), which pi's own output proves — pi v1.3.0's
//     emitted HTML contains `${totalCost.toFixed(3)}` because template.js
//     carried `$${totalCost.toFixed(3)}`,
//   * the vendored highlight.js bundle carries `$&` inside a regex char
//     class, so pi's own output re-expands the matched `{{HIGHLIGHT_JS}}`
//     placeholder at exactly that spot — the exporter reproduces that
//     artifact to stay byte-comparable,
//   * the base64 `SESSION_DATA` payload decodes to pi's sessionData shape
//     (`{header, entries, leafId, ...}`), with every entry as its pi wire
//     line in store order.

#include "coding_agent/tui/HtmlExport.hpp"
#include "coding_agent/tui/Theme.hpp"

#include "support/Json.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/ai/Message.hpp>
#include <cch/tui/TerminalColors.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace catui = cch::coding_agent::tui;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::size_t count_substring(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

/// RFC 4648 base64 decode (the test-side counterpart of the vendored
/// encoder); the SESSION_DATA payload assertion needs the actual bytes.
std::string decode_base64(std::string_view encoded) {
    const auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string decoded;
    int accumulator = 0;
    int bits = -8;
    for (const char c : encoded) {
        if (c == '=') break;
        const int value = value_of(c);
        REQUIRE(value >= 0);
        accumulator = (accumulator << 6) + value;
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return decoded;
}

/// Extracts the base64 `SESSION_DATA` payload pi's template.js decodes with
/// `atob`: the template carries it as the text content of
/// `<script id="session-data" type="application/json">`.
std::string session_data_payload(const std::string& html) {
    const std::string marker = R"(<script id="session-data" type="application/json">)";
    const auto begin = html.find(marker);
    REQUIRE(begin != std::string::npos);
    const auto end = html.find("</script>", begin + marker.size());
    REQUIRE(end != std::string::npos);
    return html.substr(begin + marker.size(), end - begin - marker.size());
}

harness::session::SessionMetadata metadata_for(const tests::TempWorkspace& workspace) {
    return harness::session::SessionMetadata{
            .session_id = "html-export-test",
            .created_at = "2026-07-05T00:00:00Z",
            .workspace = workspace.path(),
            .provider = "fake",
            .model = "fake-model",
    };
}

[[nodiscard]] harness::session::SessionStore make_store(
        const tests::TempWorkspace& workspace, const std::filesystem::path& path) {
    auto created = harness::session::SessionStore::create_new(path, metadata_for(workspace));
    REQUIRE(created.has_value());
    return std::move(*created);
}

void append_exchange(harness::session::SessionStore& store) {
    REQUIRE(store.append(ai::MessageVariant{ai::user_text_message("first question")}));
    // The session writer rejects an assistant record without the identity and
    // real timestamp a provider message carries (SessionStoreTest's contract).
    ai::AssistantMessage assistant;
    assistant.content.emplace_back(ai::text_content("first answer"));
    assistant.api = "openai-completions";
    assistant.provider = "fake";
    assistant.model = "fake-model";
    assistant.stop_reason = ai::AssistantStopReason::Stop;
    assistant.timestamp = 1718000000000;
    REQUIRE(store.append(ai::MessageVariant{std::move(assistant)}));
}

catui::HtmlExportTheme builtin_dark_export_theme() {
    auto theme =
            catui::make_html_export_theme(catui::builtin_dark_theme(), std::nullopt, catui::TerminalAppearance::Dark);
    REQUIRE(theme.has_value());
    return std::move(*theme);
}

std::string stub_session_data() {
    return R"({"header":{"type":"summary","id":"stub","cwd":"/tmp","pi":"fake"},"entries":[],"leafId":null})";
}

} // namespace

TEST_CASE("HTML export theme resolves pi's export colors and emits hex CSS variables",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    catui::ResolvedTheme theme;
    theme.name = "custom";
    theme.vars.emplace("panel", std::string{"#102030"});
    theme.export_colors.pageBg = std::string{"panel"}; // vars reference
    theme.export_colors.cardBg = std::string{""};      // terminal default
    theme.export_colors.infoBg = 196;                  // xterm palette index
    theme.colors[static_cast<std::size_t>(catui::ThemeToken::Accent)] =
            catui::RgbThemeColor{.red = 0xa7, .green = 0x98, .blue = 0xd7};

    const cch::tui::TerminalColors reported{
            .foreground = cch::tui::RgbColor{.red = 200, .green = 200, .blue = 200},
            .background = cch::tui::RgbColor{.red = 10, .green = 20, .blue = 30},
    };
    const auto resolved = catui::make_html_export_theme(theme, reported, catui::TerminalAppearance::Dark);
    REQUIRE(resolved.has_value());

    // pi `colorToHex`: lowercase `#rrggbb`, never a CSS color function.
    CHECK(resolved->page_bg == "#102030");
    CHECK(resolved->info_bg == "#ff0000"); // xterm index 196
    // pi's `resolve` maps `""` to absent, so the cardBg slot falls to the
    // derived fallback: `deriveExportColors` darkens the userMessageBg base
    // (the reported terminal background here) and keeps pi's `rgb()` spelling.
    CHECK(resolved->card_bg == "rgb(9, 17, 26)");

    bool saw_accent = false;
    for (const auto& [name, value] : resolved->css_vars) {
        CHECK(value.starts_with('#'));
        CHECK(value.size() == 7);
        CHECK(value.find("oklch(") == std::string::npos);
        CHECK(value.find("okhsl(") == std::string::npos);
        if (name == "accent") {
            saw_accent = true;
            CHECK(value == "#a798d7");
        }
    }
    CHECK(saw_accent);
}

TEST_CASE("HTML export theme maps the builtin dark theme to pi's export surfaces",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    const auto resolved = builtin_dark_export_theme();
    // The builtin dark theme's explicit `export` section, hex like pi's
    // `colorToHex` output.
    CHECK(resolved.page_bg == "#21252c");
    CHECK(resolved.card_bg == "#282c34");
    CHECK(resolved.info_bg == "#4e2f1b");
}

TEST_CASE("HTML export generate_session_html substitutes every pi template slot with JS replace semantics",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    const auto html = catui::generate_session_html(stub_session_data(), builtin_dark_export_theme());
    REQUIRE(html.has_value());
    CHECK_FALSE(html->empty());

    // Every named template slot is gone (pi's substitution contract).
    for (const std::string_view slot : {"{{CSS}}",
                 "{{JS}}",
                 "{{SESSION_DATA}}",
                 "{{MARKED_JS}}",
                 "{{THEME_VARS}}",
                 "{{BODY_BG}}",
                 "{{CONTAINER_BG}}",
                 "{{INFO_BG}}"}) {
        CAPTURE(slot);
        CHECK(html->find(slot) == std::string::npos);
    }

    // The CSS slot carries the theme: real hex variables, no placeholders.
    CHECK(html->find("--accent: #") != std::string::npos);
    CHECK(html->find("--exportPageBg: #21252c;") != std::string::npos);
    // The SESSION_DATA slot carries the base64 payload pi's template.js
    // decodes (`<script id="session-data">` textContent, then `atob`).
    CHECK(session_data_payload(*html).size() > 32);
    CHECK(html->find("atob(base64)") != std::string::npos);

    // JS `String.prototype.replace` semantics: template.js sources the cost
    // row as `$${totalCost.toFixed(3)}`, and pi's own emitted HTML contains
    // the collapsed `${totalCost.toFixed(3)}` — a literal replacement would
    // leak `$$`.
    CHECK(html->find("${totalCost.toFixed(3)}") != std::string::npos);
    CHECK(html->find("$${totalCost") == std::string::npos);

    // The vendored highlight.js bundle carries `$&` inside a regex char
    // class, so pi's own v1.3.0 output re-expands the matched
    // `{{HIGHLIGHT_JS}}` placeholder at exactly that spot. Byte parity with
    // pi reproduces the artifact rather than "fixing" it.
    CHECK(count_substring(*html, "{{HIGHLIGHT_JS}}") == 1);
    CHECK(html->find("|[-+*\\/?!{{HIGHLIGHT_JS}}|:<=>@^~]") != std::string::npos);
}

TEST_CASE("HTML export SESSION_DATA decodes to pi's sessionData shape with wire-line entries",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    auto store = make_store(workspace, session_path);
    append_exchange(store);
    const auto leaf = store.leaf_id();
    REQUIRE_FALSE(leaf.empty());

    const auto exported = catui::export_session_to_html(
            store, (workspace.path() / "out.html").string(), session_path, builtin_dark_export_theme(), {});
    REQUIRE(exported.has_value());

    const auto html = read_file(*exported);
    const auto decoded = decode_base64(session_data_payload(html));
    const auto data = support::read_json(decoded);
    REQUIRE(data.has_value());

    const auto& object = data->get_object();
    // pi's member set, no more: no `renderedTools` is emitted because the
    // harness tool renderers produce ANSI text, not HTML (pi treats the
    // member as optional and renders a tool-name header instead).
    CHECK(object.size() == 3);
    const auto& header = object.at("header").get_object();
    CHECK(header.at("type").get<std::string>() == "session");
    CHECK(header.at("id").get<std::string>() == "html-export-test");
    const auto& entries = object.at("entries").get<support::JsonValue::array_t>();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].get_object().at("type").get<std::string>() == "message");
    const auto& first_message = entries[0].get_object().at("message").get_object();
    CHECK(first_message.at("role").get<std::string>() == "user");
    const auto& first_block = first_message.at("content").get<support::JsonValue::array_t>().front();
    CHECK(first_block.get_object().at("text").get<std::string>() == "first question");
    CHECK(entries[1].get_object().at("type").get<std::string>() == "message");
    CHECK(object.at("leafId").get<std::string>() == leaf);
}

TEST_CASE("HTML export SESSION_DATA carries the optional pi AgentState members when present",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    auto store = make_store(workspace, session_path);
    append_exchange(store);

    catui::HtmlExportExtras extras;
    extras.system_prompt = "You are a test assistant.";
    extras.tools = {ai::Tool{.name = "read_file", .description = "Read a file."}};
    const auto exported = catui::export_session_to_html(
            store, (workspace.path() / "out.html").string(), session_path, builtin_dark_export_theme(), extras);
    REQUIRE(exported.has_value());
    const auto decoded = decode_base64(session_data_payload(read_file(*exported)));
    const auto data = support::read_json(decoded);
    REQUIRE(data.has_value());
    const auto& object = data->get_object();
    CHECK(object.at("systemPrompt").get<std::string>() == "You are a test assistant.");
    const auto& tools = object.at("tools").get<support::JsonValue::array_t>();
    REQUIRE(tools.size() == 1);
    CHECK(tools.front().get_object().at("name").get<std::string>() == "read_file");
    CHECK_FALSE(object.contains("renderedTools"));
}

TEST_CASE("HTML export default output name is relative to the working directory",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "deep" / "session-abc123.jsonl";
    std::error_code error;
    std::filesystem::create_directories(session_path.parent_path(), error);
    REQUIRE_FALSE(error);
    auto store = make_store(workspace, session_path);
    append_exchange(store);

    // pi `exportSessionToHtml`: `${APP_NAME}-session-${basename}.html`
    // resolved against the process working directory (pi `writeFileSync` on
    // a relative path); the C++ binary's identity is "pike". The default
    // name collides if the working directory already holds one, so clean up
    // after the assertion.
    const auto exported = catui::export_session_to_html(store, "", session_path, builtin_dark_export_theme(), {});
    REQUIRE(exported.has_value());
    CHECK(*exported == "pike-session-session-abc123.html");
    REQUIRE(std::filesystem::exists(*exported));
    const auto html = read_file(*exported);
    CHECK(html.starts_with("<!DOCTYPE html>"));
    CHECK(html.ends_with("</html>\n"));                      // the vendored template's own terminator
    CHECK(html.find("first question") == std::string::npos); // payload is base64
    std::filesystem::remove(*exported, error);
    REQUIRE_FALSE(error);
}

TEST_CASE("SessionStore export_wire_snapshot mirrors header, entries, and leaf",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    auto store = make_store(workspace, session_path);
    append_exchange(store);

    const auto snapshot = store.export_wire_snapshot();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->header.find(R"("type":"session")") != std::string::npos);
    REQUIRE(snapshot->entries.size() == 2);
    for (const auto& line : snapshot->entries) {
        // The C++ wire line leads with the entry id (the session file
        // format is Agent Core's surface); pi's emission leads with `type`.
        CHECK(line.starts_with(R"({"id":)"));
        CHECK(line.find(R"("type":"message")") != std::string::npos);
        CHECK_FALSE(line.ends_with('\n'));
    }
    CHECK(snapshot->entries[0].find(R"("first question")") != std::string::npos);
    CHECK(snapshot->leaf_id == store.leaf_id());
}
