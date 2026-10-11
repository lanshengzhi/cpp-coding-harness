#include "OAuthSelector.hpp"

#include "Theme.hpp"
#include "DynamicBorder.hpp"

#include <cch/tui/Container.hpp>
#include <cch/tui/Text.hpp>

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

constexpr std::size_t kMaxVisible = 8;

[[nodiscard]] bool is_env_source_label(std::string_view source) {
    if (source.empty()) return false;
    std::size_t begin = 0;
    while (begin < source.size()) {
        const auto comma = source.find(", ", begin);
        const auto part =
                source.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
        if (part.empty() || !std::isupper(static_cast<unsigned char>(part.front()))) return false;
        for (const char ch : part.substr(1)) {
            const auto value = static_cast<unsigned char>(ch);
            if (!std::isupper(value) && !std::isdigit(value) && ch != '_') return false;
        }
        if (comma == std::string_view::npos) return true;
        begin = comma + 2;
    }
    return false;
}

[[nodiscard]] std::string_view auth_type_label(const AuthSelectorProvider& provider) {
    if (provider.id == "openrouter" && provider.auth_type == AuthSelectorType::OAuth) return "account";
    return format_auth_selector_provider_type(provider.auth_type);
}

[[nodiscard]] std::string status_indicator(const LiveTheme& theme, const AuthSelectorProvider& provider) {
    if (!provider.status) return theme.foreground(ThemeToken::Muted, " • unconfigured");
    if (provider.status->type != provider.auth_type) {
        const std::string label =
                provider.status->type == AuthSelectorType::OAuth
                        ? (provider.id == "openrouter" ? "account configured" : "subscription configured")
                        : "API key configured";
        return theme.foreground(ThemeToken::Muted, " • ") + theme.foreground(ThemeToken::Warning, label);
    }
    if (!provider.status->source || *provider.status->source == "OAuth" ||
        *provider.status->source == "stored credential") {
        return theme.foreground(ThemeToken::Success, " ✓ configured");
    }
    const auto& source = *provider.status->source;
    if (is_env_source_label(source)) return theme.foreground(ThemeToken::Success, " ✓ env: " + source);
    return theme.foreground(ThemeToken::Success, " ✓ " + source);
}

[[nodiscard]] std::string provider_identity(const AuthSelectorProvider& provider) {
    return provider.id + "/" + std::string{auth_selector_type_wire_name(provider.auth_type)};
}

[[nodiscard]] std::vector<cch::tui::SelectItem> build_items(
        const LiveTheme& theme, const std::vector<AuthSelectorProvider>& providers) {
    std::size_t distinct_types = 0;
    for (const auto& provider : providers)
        distinct_types |= provider.auth_type == AuthSelectorType::OAuth ? 1U : 2U;
    const bool show_type_labels = distinct_types == 3U;

    std::vector<cch::tui::SelectItem> items;
    items.reserve(providers.size());
    for (const auto& provider : providers) {
        std::string label = provider.name;
        if (show_type_labels)
            label += theme.foreground(ThemeToken::Muted, " [" + std::string{auth_type_label(provider)} + "]");
        label += status_indicator(theme, provider);
        std::string search_text = provider.name + " " + provider.id + " " +
                                  std::string{auth_selector_type_wire_name(provider.auth_type)} + " " +
                                  provider.method_name.value_or("");
        items.push_back(cch::tui::SelectItem{
                .value = provider_identity(provider),
                .label = std::move(label),
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
    : providers_(std::move(providers)), on_select_(std::move(on_select)), on_cancel_(std::move(on_cancel)),
      title_(theme.foreground(ThemeToken::Accent,
              "\x1b[1m" +
                      std::string{mode == AuthSelectorMode::Login ? "Select provider to configure:"
                                                                  : "Select provider to logout:"} +
                      "\x1b[22m")),
      border_style_(theme.foreground_hook(ThemeToken::Border)),
      select_list_(build_items(theme, providers_),
              cch::tui::SelectListOptions{
                      .max_visible = kMaxVisible,
                      .theme = theme.select_list_theme(),
                      .on_select = [this](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                          const auto provider = std::find_if(
                                  providers_.begin(), providers_.end(), [&item](const AuthSelectorProvider& candidate) {
                                      return provider_identity(candidate) == item.value;
                                  });
                          if (provider != providers_.end() && on_select_) on_select_(provider->id, provider->auth_type);
                          return {};
                      },
                      .on_cancel = [this]() -> support::ExpectedVoid {
                          if (on_cancel_) on_cancel_();
                          return {};
                      },
                      .keybindings = std::move(keybindings),
              },
              {},
              providers_.empty() ? (mode == AuthSelectorMode::Login ? "  No providers available"
                                                                    : "  No providers logged in. Use /login first.")
                                 : "  No matching providers") {
    if (!initial_search.empty()) select_list_.set_filter(std::move(initial_search));
}

support::Expected<cch::tui::RenderResult> OAuthSelectorComponent::render(std::size_t width) {
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines)
            result.lines.push_back(std::move(line));
        return {};
    };
    const auto border = [this](std::string line) {
        return border_style_ ? border_style_(std::move(line)) : std::move(line);
    };
    DynamicBorder top_border(border);
    cch::tui::Spacer spacer(1);
    cch::tui::Text title(title_, 1, 0);
    if (auto added = append(top_border); !added) return std::unexpected(added.error());
    if (auto added = append(spacer); !added) return std::unexpected(added.error());
    if (auto added = append(title); !added) return std::unexpected(added.error());
    if (auto added = append(spacer); !added) return std::unexpected(added.error());
    list_row_offset_ = result.lines.size();
    auto list = select_list_.render(width);
    if (!list) return std::unexpected(list.error());
    for (auto& line : list->lines)
        result.lines.push_back(std::move(line));
    DynamicBorder bottom_border(border);
    if (auto added = append(bottom_border); !added) return std::unexpected(added.error());
    return result;
}

void OAuthSelectorComponent::invalidate() { select_list_.invalidate(); }

cch::tui::InputAdmissionOutcome OAuthSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key != nullptr && key->type == cch::tui::KeyEventType::Release)
        return cch::tui::InputAdmissionOutcome::Unhandled;
    return select_list_.handle_input(input);
}

void OAuthSelectorComponent::set_focused(bool focused) { select_list_.set_focused(focused); }
bool OAuthSelectorComponent::focused() const { return select_list_.focused(); }
std::optional<cch::tui::CursorPosition> OAuthSelectorComponent::cursor_location() const {
    auto cursor = select_list_.cursor_location();
    if (!cursor) return std::nullopt;
    cursor->row += list_row_offset_;
    return cursor;
}

} // namespace cch::coding_agent::tui
