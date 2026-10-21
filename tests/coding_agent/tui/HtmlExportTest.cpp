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
//     class, so pi's own output contains exactly one literal
//     `{{HIGHLIGHT_JS}}` at that spot — the exporter must reproduce that
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
/// `atob`: `const sessionData = "<payload>";`.
std::string session_data_payload(const std::string& html) {
    const std::string marker = "const sessionData = \"";
    const auto begin = html.find(marker);
    REQUIRE(begin != std::string::npos);
    const auto end = html.find('"', begin + marker.size());
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
    REQUIRE(store.append(ai::MessageVariant{ai::assistant_text_message("first answer")}));
}

catui::HtmlExportTemplates vendored_templates() {
    return catui::HtmlExportTemplates{
            .dir = std::filesystem::path{CCH_SOURCE_DIR} / "third_party/pi-export-html",
    };
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
    theme.export_colors.page_bg = std::string{"panel"}; // vars reference
    theme.export_colors.card_bg = std::string{""};      // terminal default
    theme.export_colors.info_bg = 196;                  // xterm palette index
    theme.colors[static_cast<std::size_t>(catui::ThemeToken::Accent)] =
            catui::RgbThemeColor{.red = 0xa7, .green = 0x98, .blue = 0xd7};

    const cch::tui::TerminalColors reported{
            .foreground = cch::tui::RgbColor{.red = 200, .green = 200, .blue = 200},
            .background = cch::tui::RgbColor{.red = 10, .green = 20, .blue = 30},
    };
    const auto resolved =
            catui::make_html_export_theme(theme, reported, cch::tui::TerminalAppearance::Dark);
    REQUIRE(resolved.has_value());

    // pi `colorToHex`: lowercase `#rrggbb`, never a CSS color function.
    CHECK(resolved->page_bg == "#102030");
    CHECK(resolved->card_bg == "#0a141e"); // the reported background verbatim
    CHECK(resolved->info_bg == "#ff0000"); // xterm index 196

    bool saw_accent = false;
    bool saw_export_page = false;
    for (const auto& [name, value] : resolved->css_vars) {
        CHECK(value.starts_with('#'));
        CHECK(value.size() == 7);
        if (name == "--accent") {
            saw_accent = true;
            CHECK(value == "#a798d7");
        }
        if (name == "--exportPageBg") {
            saw_export_page = true;
            CHECK(value == "#102030");
        }
        CHECK(value.find("oklch(") == std::string::npos);
        CHECK(value.find("okhsl(") == std::string::npos);
    }
    CHECK(saw_accent);
    CHECK(saw_export_page);
}

TEST_CASE("HTML export theme maps the builtin dark theme to pi's export surfaces",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    const auto resolved = catui::make_html_export_theme(
            catui::builtin_dark_theme(), std::nullopt, cch::tui::TerminalAppearance::Dark);
    REQUIRE(resolved.has_value());
    CHECK(resolved->page_bg == "#100014");
    CHECK(resolved->card_bg == "#150019");
    CHECK(resolved->info_bg == "#4d3300");
    bool saw_export_page = false;
    for (const auto& [name, value] : resolved->css_vars) {
        if (name == "--exportPageBg") {
            saw_export_page = true;
            CHECK(value == "#100014");
        }
    }
    CHECK(saw_export_page);
}

TEST_CASE("generate_session_html substitutes every pi template slot with JS replace semantics",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    const auto html = catui::generate_session_html(stub_session_data(), vendored_templates(),
            catui::make_html_export_theme(
                    catui::builtin_dark_theme(), std::nullopt, cch::tui::TerminalAppearance::Dark)
                    .value());
    REQUIRE(html.has_value());
    CHECK_FALSE(html->empty());

    // Every named template slot is gone (pi's substitution contract).
    for (const std::string_view slot : {"{{CSS}}", "{{JS}}", "{{SESSION_DATA}}", "{{MARKED_JS}}",
                 "{{THEME_VARS}}", "{{BODY_BG}}", "{{CONTAINER_BG}}", "{{INFO_BG}}"}) {
        CAPTURE(slot);
        CHECK(html->find(slot) == std::string::npos);
    }

    // The CSS slot carries the theme: real hex variables, no placeholders.
    CHECK(html->find("--accent: #") != std::string::npos);
    CHECK(html->find("--exportPageBg: #100014;") != std::string::npos);
    // The SESSION_DATA slot carries the base64 payload pi's template.js
    // decodes (`const sessionData = "...";`).
    CHECK(session_data_payload(*html).size() > 32);
    // The vendor bundles landed in their own slots: the client-side decode
    // call pi's template.js makes on the injected payload is present.
    CHECK(html->find("atob(sessionData)") != std::string::npos);

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

TEST_CASE("SESSION_DATA decodes to pi's sessionData shape with wire-line entries",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    auto store = make_store(workspace, session_path);
    append_exchange(store);
    const auto leaf = store.leaf_id();
    REQUIRE_FALSE(leaf.empty());

    const auto exported = catui::export_session_to_html(store,
            (workspace.path() / "out.html").string(), session_path,
            catui::make_html_export_theme(
                    catui::builtin_dark_theme(), std::nullopt, cch::tui::TerminalAppearance::Dark)
                    .value(),
            {});
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
    CHECK(header.at("type").get<std::string>() == "summary");
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

TEST_CASE("SESSION_DATA carries the optional pi AgentState members when present",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    auto store = make_store(workspace, session_path);
    append_exchange(store);

    catui::HtmlExportExtras extras;
    extras.system_prompt = "You are a test assistant.";
    extras.tools = {ai::Tool{.name = "read_file", .description = "Read a file."}};
    const auto exported = catui::export_session_to_html(store,
            (workspace.path() / "out.html").string(), session_path,
            catui::make_html_export_theme(
                    catui::builtin_dark_theme(), std::nullopt, cch::tui::TerminalAppearance::Dark)
                    .value(),
            extras);
    REQUIRE(exported.has_value());
    const auto decoded = decode_base64(session_data_payload(read_file(*exported)));
    const auto data = support::read_json(decoded);
    REQUIRE(data.has_value());
    const auto& object = data->get_object();
    CHECK(object.at("systemPrompt").get<std::string>() == "You are a test assistant.");
    const auto& tools = object.at("tools").get<support::JsonValue::array_t>();
    REQUIRE(tools.size() == 1);
    CHECK(tools.front().get_object().at("name").get<std::string>() == "read_file");
    CHECK(object.contains("renderedTools") == false);
}

TEST_CASE("empty destination uses pi's default output name next to the session file",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "deep" / "session-abc123.jsonl";
    std::error_code error;
    std::filesystem::create_directories(session_path.parent_path(), error);
    REQUIRE_FALSE(error);
    auto store = make_store(workspace, session_path);
    append_exchange(store);

    const auto exported =
            catui::export_session_to_html(store, "", session_path,
                    catui::make_html_export_theme(
                            catui::builtin_dark_theme(), std::nullopt,
                            cch::tui::TerminalAppearance::Dark)
                            .value(),
                    {});
    REQUIRE(exported.has_value());
    // pi `exportSessionToHtml`: `${APP_NAME}-session-${basename}.html` in the
    // session file's directory; the C++ binary's identity is "pike".
    CHECK(*exported == (workspace.path() / "deep" / "pike-session-session-abc123.html").string());
    CHECK(std::filesystem::exists(*exported));
    const auto html = read_file(*exported);
    CHECK(html.starts_with("<!DOCTYPE html>"));
    CHECK(html.ends_with("</html>"));
}

TEST_CASE("generate_session_html surfaces missing template assets as an error",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    const auto html = catui::generate_session_html(stub_session_data(),
            catui::HtmlExportTemplates{.dir = "/nonexistent/export-html"},
            catui::make_html_export_theme(
                    catui::builtin_dark_theme(), std::nullopt, cch::tui::TerminalAppearance::Dark)
                    .value());
    REQUIRE_FALSE(html.has_value());
}

TEST_CASE("SessionStore export_wire_snapshot mirrors header, entries, and leaf",
        "[coding_agent][tui][export][html][issue1044][spec]") {
    tests::TempWorkspace workspace;
    const auto session_path = workspace.path() / "session.jsonl";
    auto store = make_store(workspace, session_path);
    append_exchange(store);

    const auto snapshot = store.export_wire_snapshot();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->header.find(R"("type":"summary")") != std::string::npos);
    REQUIRE(snapshot->entries.size() == 2);
    for (const auto& line : snapshot->entries) {
        CHECK(line.starts_with(R"({"type":)"));
        CHECK_FALSE(line.ends_with('\n'));
    }
    CHECK(snapshot->entries[0].find(R"("first question")") != std::string::npos);
    CHECK(snapshot->leaf_id == store.leaf_id());
}
