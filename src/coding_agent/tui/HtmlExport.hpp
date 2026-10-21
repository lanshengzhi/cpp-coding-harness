#pragma once

#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/ThemeColorMath.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/ai/Tool.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/TerminalColors.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

/// pi `getResolvedThemeColors` + `getThemeExportColors`, resolved for the
/// HTML export template: every theme token as a CSS custom property in pi's
/// emission order (foreground-slot tokens, then background-slot tokens, each
/// in theme token order), plus the three export backgrounds with pi's
/// explicit-or-derived fallback already applied.
struct HtmlExportTheme {
    std::vector<std::pair<std::string, std::string>> css_vars;
    std::string page_bg;
    std::string card_bg;
    std::string info_bg;
};

/// Resolve the committed active theme for `/export`'s HTML lane.
/// `terminal_colors` are the terminal-reported defaults (pi's
/// `terminalColors`) used for `""` (terminal-default) tokens; the appearance
/// only picks the pi guessed-default pair when the theme declares no
/// detectable colors of its own.
[[nodiscard]] support::Expected<HtmlExportTheme> make_html_export_theme(const ResolvedTheme& theme,
        const std::optional<cch::tui::TerminalColors>& terminal_colors,
        TerminalAppearance appearance);

/// pi `AgentState` subset the HTML payload carries: `systemPrompt` and the
/// active tools' definitions (`sessionData.tools`). Absent/empty fields are
/// omitted from the payload exactly like pi's `JSON.stringify` drops
/// `undefined`.
struct HtmlExportExtras {
    std::optional<std::string> system_prompt;
    std::vector<ai::Tool> tools;
};

/// Build pi's `sessionData` JSON document: `{header, entries, leafId}` plus
/// the optional `systemPrompt`/`tools` members. `entries` is pi's
/// `SessionManager.getEntries()` — every entry of the session, each as its pi
/// wire line — and `leafId` is the active leaf's id or null.
[[nodiscard]] support::Expected<std::string> build_html_session_data_json(
        cch::harness::session::SessionStore& store, const HtmlExportExtras& extras);

/// pi `generateHtml`: substitute the vendored pi template (third_party/
/// pi-export-html, byte-for-byte pi v1.3.0) with the base64 session-data
/// payload, the theme variables, and the export background colors.
[[nodiscard]] support::Expected<std::string> generate_session_html(
        std::string_view session_data_json, const HtmlExportTheme& theme);

/// pi `exportSessionToHtml`: an empty `output_path` selects pi's default
/// `pike-session-<basename>.html`; the destination is written through the
/// workspace filesystem like the JSONL lane. Returns the written path.
[[nodiscard]] support::Expected<std::string> export_session_to_html(cch::harness::session::SessionStore& store,
        std::string_view output_path,
        const std::filesystem::path& session_file,
        const HtmlExportTheme& theme,
        const HtmlExportExtras& extras);

} // namespace cch::coding_agent::tui
