#pragma once

#include "coding_agent/tui/SearchableSelectList.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

enum class AuthSelectorType { OAuth, ApiKey };
enum class AuthSelectorMode { Login, Logout };

[[nodiscard]] inline std::string_view auth_selector_type_wire_name(AuthSelectorType type) {
    return type == AuthSelectorType::OAuth ? "oauth" : "api_key";
}

struct AuthSelectorStatus {
    AuthSelectorType type{AuthSelectorType::ApiKey};
    std::optional<std::string> source{std::nullopt};
};

struct AuthSelectorProvider {
    std::string id{};
    std::string name{};
    AuthSelectorType auth_type{AuthSelectorType::OAuth};
    std::optional<std::string> method_name{std::nullopt};
    std::optional<AuthSelectorStatus> status{std::nullopt};
    bool has_login{false};
};

[[nodiscard]] inline std::string_view format_auth_selector_provider_type(AuthSelectorType type) {
    return type == AuthSelectorType::OAuth ? "subscription" : "API key";
}

using AuthProviderSelectSink = std::move_only_function<void(std::string, AuthSelectorType)>;
using AuthProviderCancelSink = std::move_only_function<void()>;

class OAuthSelectorComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    OAuthSelectorComponent(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        AuthSelectorMode mode,
        std::vector<AuthSelectorProvider> providers,
        AuthProviderSelectSink on_select,
        AuthProviderCancelSink on_cancel,
        std::string initial_search = {});
    OAuthSelectorComponent(OAuthSelectorComponent&&) = delete;
    OAuthSelectorComponent& operator=(OAuthSelectorComponent&&) = delete;
    ~OAuthSelectorComponent() override = default;
    OAuthSelectorComponent(const OAuthSelectorComponent&) = delete;
    OAuthSelectorComponent& operator=(const OAuthSelectorComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override;
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    std::vector<AuthSelectorProvider> providers_;
    AuthProviderSelectSink on_select_;
    AuthProviderCancelSink on_cancel_;
    std::string title_;
    cch::tui::TextStyleHook border_style_;
    SearchableSelectList select_list_;
    std::size_t list_row_offset_{4};
};

} // namespace cch::coding_agent::tui
