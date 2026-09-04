#include "OAuthSelector.hpp"

#include "Theme.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kMaxVisible = 8;

/// pi's env-var source test (`/^[A-Z][A-Z0-9_]*(?:, [A-Z][A-Z0-9_]*)*$/`):
/// one or more comma-space-separated SHOUTY env var names.
[[nodiscard]] bool is_env_source_label(std::string_view source) {
    if (source.empty()) return false;
    std::size_t begin = 0;
    while (begin < source.size()) {
        const auto comma = source.find(", ", begin);
        const auto part = source.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
        if (part.empty() || !std::isupper(static_cast<unsigned char>(part.front()))) {
            return false;
        }
        for (const char ch : part.substr(1)) {
            const auto value = static_cast<unsigned char>(ch);
            if (!std::isupper(value) && !std::isdigit(value) && ch != '_') return false;
        }
        if (comma == std::string_view::npos) return true;
        begin = comma + 2;
    }
    return false;
}

/// pi `formatStatusIndicator`, branch for branch.
[[nodiscard]] std::string status_indicator(const LiveTheme& theme, const AuthSelectorProvider& provider) {
    if (!provider.status) {
        return theme.foreground(ThemeToken::Muted, " • unconfigured");
    }
    if (provider.status->type != provider.auth_type) {
        const std::string label = provider.status->type == AuthSelectorType::OAuth
            ? "subscription configured"
            : "API key configured";
        return theme.foreground(ThemeToken::Muted, " • ") + theme.foreground(ThemeToken::Warning, label);
    }
    if (!provider.status->source || *provider.status->source == "OAuth" ||
        *provider.status->source == "stored credential") {
        return theme.foreground(ThemeToken::Success, " ✓ configured");
    }
    const auto& source = *provider.status->source;
    if (is_env_source_label(source)) {
        return theme.foreground(ThemeToken::Success, " ✓ env: " + source);
    }
    return theme.foreground(ThemeToken::Success, " ✓ " + source);
}

/// One `SelectItem` per provider row, in provider order. The label carries
/// the full visible row (display name, the optional `[subscription]` /
/// `[API key]` type label when the list mixes auth types, and the colored
/// status indicator) so rows render exactly as before; `value` holds the
/// `providers_` index so selection resolves back to the provider; the hidden
/// `search_text` keeps the historical name + id + auth type + method-name
/// search surface.
[[nodiscard]] std::vector<cch::tui::SelectItem> build_items(
        const LiveTheme& theme, const std::vector<AuthSelectorProvider>& providers) {
    // pi: the auth-type labels render only when the list mixes auth types.
    std::size_t distinct_types = 0;
    for (const auto& provider : providers) {
        distinct_types |= provider.auth_type == AuthSelectorType::OAuth ? 1U : 2U;
    }
    const bool show_type_labels = distinct_types == 3U;

    std::vector<cch::tui::SelectItem> items;
    items.reserve(providers.size());
    for (std::size_t index = 0; index < providers.size(); ++index) {
        const auto& provider = providers[index];
        std::string label = provider.name;
        if (show_type_labels) {
            label += theme.foreground(ThemeToken::Muted,
                    " [" + std::string{format_auth_selector_provider_type(provider.auth_type)} + "]");
        }
        label += status_indicator(theme, provider);
        std::string search_text = provider.name + " " + provider.id + " " +
                                  std::string{auth_selector_type_wire_name(provider.auth_type)} + " " +
                                  provider.method_name.value_or("");
        items.push_back(cch::tui::SelectItem{
                .value = std::to_string(index),
                .label = std::move(label),
                .description = std::nullopt,
                .search_text = std::move(search_text),
        });
    }
    return items;
}

} // namespace

OAuthSelectorComponent::OAuthSelectorComponent(const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        AuthSelectorMode mode,
        std::vector<AuthSelectorProvider> providers,
        AuthProviderSelectSink on_select,
        AuthProviderCancelSink on_cancel,
        std::string initial_search)
    : theme_(theme), keybindings_(std::move(keybindings)), mode_(mode), providers_(std::move(providers)),
      on_select_(std::move(on_select)), on_cancel_(std::move(on_cancel)),
      select_list_(build_items(theme_, providers_),
              cch::tui::SelectListOptions{
                      .max_visible = kMaxVisible,
                      .theme = theme_.select_list_theme(),
                      .on_select = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                          std::size_t index = 0;
                          const auto parsed =
                                  std::from_chars(item.value.data(), item.value.data() + item.value.size(), index);
                          if (parsed.ec == std::errc{} && index < providers_.size() && on_select_) {
                              const auto& provider = providers_[index];
                              on_select_(provider.id, provider.auth_type);
                          }
                          return {};
                      },
                      .on_cancel = [this]() -> support::ExpectedVoid {
                          if (on_cancel_) on_cancel_();
                          return {};
                      },
                      .keybindings = keybindings_,
                      .enable_search = true,
                      .initial_search = initial_search.empty() ? std::nullopt
                                                               : std::optional<std::string>{std::move(initial_search)},
                      .title = theme_.foreground(ThemeToken::Accent,
                              "\x1b[1m" +
                                      std::string{mode_ == AuthSelectorMode::Login ? "Select provider to configure:"
                                                                                   : "Select provider to logout:"} +
                                      "\x1b[22m"),
                      .border_hook = theme_.foreground_hook(ThemeToken::Border),
                      // The SelectList's generic no-match row ("No matching
                      // commands") carries the mode-specific message: the
                      // selector's pi wording depends only on the mode and
                      // whether any provider exists, both fixed at
                      // construction.
                      .no_match_text = providers_.empty()
                                               ? (mode_ == AuthSelectorMode::Login
                                                                 ? "  No providers available"
                                                                 : "  No providers logged in. Use /login first.")
                                               : "  No matching providers",
              }) {}

support::Expected<cch::tui::RenderResult> OAuthSelectorComponent::render(std::size_t width) {
    return select_list_.render(width);
}

void OAuthSelectorComponent::invalidate() { select_list_.invalidate(); }

void OAuthSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    select_list_.handle_input(input);
}

void OAuthSelectorComponent::set_focused(bool focused) { select_list_.set_focused(focused); }

bool OAuthSelectorComponent::focused() const { return select_list_.focused(); }

std::optional<cch::tui::CursorPosition> OAuthSelectorComponent::cursor_location() const {
    // The SelectList tracks the search row inside its own chrome (border,
    // spacers, title), so the component adds no offset of its own.
    return select_list_.cursor_location();
}

} // namespace cch::coding_agent::tui
