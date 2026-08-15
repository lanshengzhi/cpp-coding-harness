#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Component.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace cch::coding_agent::tui {

/// Passive footer presentation data (pi `footer.ts` render inputs computed
/// from the session snapshot, the model runtime, and the git metadata).
struct FooterData {
    /// Session cwd (pi `sessionManager.getCwd()`).
    std::filesystem::path cwd;
    /// Git branch for the footer's `(branch)` suffix; nullopt outside a repo
    /// (pi `getGitBranch`), `"detached"` on a detached HEAD.
    std::optional<std::string> git_branch{std::nullopt};
    /// Cumulative usage over the session (pi `usage-totals.ts`
    /// `addUsageToTotals` over the session entries).
    std::size_t input{0};
    std::size_t output{0};
    std::size_t cache_read{0};
    std::size_t cache_write{0};
    double cost{0};
    /// Prompt-cache hit rate of the latest assistant message, percent (pi
    /// `latestCacheHitRate`); nullopt when the latest prompt had no tokens.
    std::optional<double> cache_hit_rate{std::nullopt};
    /// Estimated context tokens; nullopt renders `?` (pi `getContextUsage`
    /// `tokens: null` — unknown until the next LLM response after a
    /// compaction).
    std::optional<std::size_t> context_tokens{std::nullopt};
    /// Active model context window (0 when no model is selected).
    std::size_t context_window{0};
    /// pi footer `autoCompactEnabled` — renders the ` (auto)` context suffix.
    bool auto_compact_enabled{true};
    /// Active model provider id (pi `state.model?.provider`) — rendered as
    /// the `(provider)` prefix when multiple providers are available.
    std::string provider;
    /// Active model id (pi `state.model?.id || "no-model"`).
    std::string model_id;
    /// Active thinking level wire name (`off`..`max`).
    std::string thinking_level{"off"};
    /// Whether the active model supports reasoning.
    bool model_reasoning{false};
    /// pi `usingSubscription`: kimi-coding, or any provider authenticating
    /// through OAuth — renders the ` (sub)` cost marker.
    bool using_subscription{false};
    /// Unique providers with available models (pi
    /// `getAvailableProviderCount`); >1 renders the `(provider)` prefix.
    std::size_t available_provider_count{0};
};

/// pi `footer.ts` `formatTokens`: compact token counts for the stats line
/// (`1234` -> `1.2k`, `34567` -> `35k`, `1234567` -> `1.2M`).
[[nodiscard]] std::string format_tokens(std::size_t count);

/// pi `footer.ts` `formatCwdForFooter`: a cwd inside the home directory
/// renders as `~` / `~/<relative>`, anything else as the cwd.
[[nodiscard]] std::string format_cwd_for_footer(
    const std::filesystem::path& cwd,
    const std::optional<std::filesystem::path>& home);

/// pi `FooterComponent`: the footer's two-line layout — the dim
/// cwd/git-branch line and the stats line (usage totals, cache hit rate,
/// cost with the `(sub)` marker, colored context percent, and the
/// right-aligned model name with thinking level and provider prefix). The
/// extension-statuses and xp lines stay out of the subset (G2).
class Footer final : public cch::tui::Component {
public:
    explicit Footer(const LiveTheme& theme) : theme_(theme) {}

    void set_data(FooterData data) { data_ = std::move(data); }
    [[nodiscard]] const FooterData& data() const { return data_; }

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override {}

private:
    const LiveTheme& theme_; // must outlive this component.
    FooterData data_;
};

} // namespace cch::coding_agent::tui
