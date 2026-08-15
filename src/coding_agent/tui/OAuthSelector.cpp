#include "OAuthSelector.hpp"

#include "DynamicBorder.hpp"
#include "Theme.hpp"

#include <cch/tui/Fuzzy.hpp>
#include <cch/tui/TruncatedText.hpp>

#include <cch/support/Error.hpp>
#include <algorithm>
#include <cctype>
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

} // namespace

OAuthSelectorComponent::OAuthSelectorComponent(
    const LiveTheme& theme,
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
    AuthSelectorMode mode,
    std::vector<AuthSelectorProvider> providers,
    AuthProviderSelectSink on_select,
    AuthProviderCancelSink on_cancel,
    std::string initial_search)
    : theme_(theme),
      keybindings_(std::move(keybindings)),
      mode_(mode),
      providers_(std::move(providers)),
      on_select_(std::move(on_select)),
      on_cancel_(std::move(on_cancel)),
      search_input_(
          cch::tui::InputOptions{.keybindings = keybindings_},
          [this](std::string) { confirm_selection(); },
          {}) {
    // pi: the auth-type labels render only when the list mixes auth types.
    std::size_t distinct_types = 0;
    for (const auto& provider : providers_) {
        distinct_types |= provider.auth_type == AuthSelectorType::OAuth ? 1U : 2U;
    }
    show_type_labels_ = distinct_types == 3U;
    search_input_.set_value(std::move(initial_search));
}

std::vector<const AuthSelectorProvider*> OAuthSelectorComponent::filtered() const {
    std::vector<const AuthSelectorProvider*> all;
    all.reserve(providers_.size());
    for (const auto& provider : providers_) all.push_back(&provider);
    const auto query = search_input_.value();
    if (query.empty()) return all;
    return cch::tui::fuzzy_filter(std::move(all), query, [](const AuthSelectorProvider* provider) {
        return provider->name + " " + provider->id + " " +
            std::string{auth_selector_type_wire_name(provider->auth_type)} + " " +
            provider->method_name.value_or("");
    });
}

std::string OAuthSelectorComponent::status_indicator(
    const AuthSelectorProvider& provider) const {
    // pi `formatStatusIndicator`, branch for branch.
    if (!provider.status) {
        return theme_.foreground(ThemeToken::Muted, " • unconfigured");
    }
    if (provider.status->type != provider.auth_type) {
        const std::string label = provider.status->type == AuthSelectorType::OAuth
            ? "subscription configured"
            : "API key configured";
        return theme_.foreground(ThemeToken::Muted, " • ") +
            theme_.foreground(ThemeToken::Warning, label);
    }
    if (!provider.status->source || *provider.status->source == "OAuth" ||
        *provider.status->source == "stored credential") {
        return theme_.foreground(ThemeToken::Success, " ✓ configured");
    }
    const auto& source = *provider.status->source;
    if (is_env_source_label(source)) {
        return theme_.foreground(ThemeToken::Success, " ✓ env: " + source);
    }
    return theme_.foreground(ThemeToken::Success, " ✓ " + source);
}

void OAuthSelectorComponent::confirm_selection() {
    const auto filtered_providers = filtered();
    if (selected_index_ >= filtered_providers.size()) return;
    const auto* provider = filtered_providers[selected_index_];
    if (on_select_) on_select_(provider->id, provider->auth_type);
}

support::Expected<cch::tui::RenderResult> OAuthSelectorComponent::render(std::size_t width) {
    cch::tui::RenderResult result;
    const auto append = [&result, width](cch::tui::Component& component) -> support::ExpectedVoid {
        auto rendered = component.render(width);
        if (!rendered) return std::unexpected(rendered.error());
        for (auto& line : rendered->lines) result.lines.push_back(std::move(line));
        return {};
    };
    const auto append_text = [&append](std::string text) -> support::ExpectedVoid {
        cch::tui::TruncatedText line(std::move(text), 1, 0);
        return append(line);
    };

    // pi's composition: border / spacer / bold accent title / spacer / search
    // input / spacer / list (+ scroll info, empty message) / spacer / border.
    DynamicBorder top_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(top_border); !appended) return std::unexpected(appended.error());
    if (auto appended = append_text(""); !appended) return std::unexpected(appended.error());
    {
        const std::string title = mode_ == AuthSelectorMode::Login
            ? "Select provider to configure:"
            : "Select provider to logout:";
        if (auto appended = append_text(
                theme_.foreground(ThemeToken::Accent, "\x1b[1m" + title + "\x1b[22m"));
            !appended) {
            return std::unexpected(appended.error());
        }
    }
    if (auto appended = append_text(""); !appended) return std::unexpected(appended.error());
    if (auto appended = append(search_input_); !appended) return std::unexpected(appended.error());
    if (auto appended = append_text(""); !appended) return std::unexpected(appended.error());

    const auto filtered_providers = filtered();
    const std::size_t clamped_index = filtered_providers.empty()
        ? 0
        : std::min(selected_index_, filtered_providers.size() - 1);
    const std::size_t start = filtered_providers.size() <= kMaxVisible
        ? 0
        : std::min(
              clamped_index > kMaxVisible / 2 ? clamped_index - kMaxVisible / 2 : 0,
              filtered_providers.size() - kMaxVisible);
    const std::size_t end = std::min(start + kMaxVisible, filtered_providers.size());

    for (std::size_t index = start; index < end; ++index) {
        const auto& provider = *filtered_providers[index];
        const bool selected = index == clamped_index;
        const std::string type_label = show_type_labels_
            ? theme_.foreground(
                  ThemeToken::Muted,
                  " [" + std::string{format_auth_selector_provider_type(provider.auth_type)} + "]")
            : "";
        const std::string line = selected
            ? theme_.foreground(ThemeToken::Accent, "→ ") +
                theme_.foreground(ThemeToken::Accent, provider.name) + type_label +
                status_indicator(provider)
            : "  " + theme_.foreground(ThemeToken::Text, provider.name) + type_label +
                status_indicator(provider);
        if (auto appended = append_text(line); !appended) return std::unexpected(appended.error());
    }
    if (start > 0 || end < filtered_providers.size()) {
        if (auto appended = append_text(theme_.foreground(
                ThemeToken::Muted,
                "  (" + std::to_string(clamped_index + 1) + "/" +
                    std::to_string(filtered_providers.size()) + ")"));
            !appended) {
            return std::unexpected(appended.error());
        }
    }
    if (filtered_providers.empty()) {
        const std::string message = providers_.empty()
            ? (mode_ == AuthSelectorMode::Login
                   ? "No providers available"
                   : "No providers logged in. Use /login first.")
            : "No matching providers";
        if (auto appended = append_text(theme_.foreground(ThemeToken::Muted, "  " + message));
            !appended) {
            return std::unexpected(appended.error());
        }
    }

    if (auto appended = append_text(""); !appended) return std::unexpected(appended.error());
    DynamicBorder bottom_border(theme_.foreground_hook(ThemeToken::Border));
    if (auto appended = append(bottom_border); !appended) return std::unexpected(appended.error());
    return result;
}

void OAuthSelectorComponent::invalidate() {
    search_input_.invalidate();
}

void OAuthSelectorComponent::handle_input(const cch::tui::InputEventVariant& input) {
    const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
    if (key == nullptr || key->type == cch::tui::KeyEventType::Release) return;

    if (keybindings_->matches(*key, "tui.select.up")) {
        selected_index_ = selected_index_ == 0 ? 0 : selected_index_ - 1;
        return;
    }
    if (keybindings_->matches(*key, "tui.select.down")) {
        const auto count = filtered().size();
        if (count != 0) selected_index_ = std::min(count - 1, selected_index_ + 1);
        return;
    }
    if (keybindings_->matches(*key, "tui.select.confirm")) {
        confirm_selection();
        return;
    }
    if (keybindings_->matches(*key, "tui.select.cancel")) {
        if (on_cancel_) on_cancel_();
        return;
    }
    // pi: everything else types into the search input and re-filters.
    search_input_.handle_input(input);
    // pi clamps the selection into the filtered range on every filter change.
    const auto count = filtered().size();
    if (count == 0) {
        selected_index_ = 0;
    } else if (selected_index_ >= count) {
        selected_index_ = count - 1;
    }
}

void OAuthSelectorComponent::set_focused(bool focused) {
    search_input_.set_focused(focused);
}

bool OAuthSelectorComponent::focused() const {
    return search_input_.focused();
}

std::optional<cch::tui::CursorPosition> OAuthSelectorComponent::cursor_location() const {
    auto cursor = search_input_.cursor_location();
    if (!cursor) return std::nullopt;
    // Rows before the search input: border, spacer, title, spacer.
    cursor->row += 4;
    return cursor;
}

} // namespace cch::coding_agent::tui
