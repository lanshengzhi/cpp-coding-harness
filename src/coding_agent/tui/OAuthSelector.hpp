#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// pi's `"oauth" | "api_key"` auth-type vocabulary.
enum class AuthSelectorType { OAuth, ApiKey };
/// pi's `"login" | "logout"` selector mode.
enum class AuthSelectorMode { Login, Logout };

[[nodiscard]] inline std::string_view auth_selector_type_wire_name(AuthSelectorType type) {
    return type == AuthSelectorType::OAuth ? "oauth" : "api_key";
}

/// Source/type-only stored-credential status for one provider row (pi
/// `AuthCheck`; never resolves key values).
struct AuthSelectorStatus {
    AuthSelectorType type{AuthSelectorType::ApiKey};
    std::optional<std::string> source{std::nullopt};
};

/// One provider row in the selector (pi `AuthSelectorProvider`). The optional
/// method name feeds fuzzy search only (pi `method?.name`); `has_login`
/// carries pi's `method?.login` presence for the api-key dialog branch.
struct AuthSelectorProvider {
    std::string id{};
    std::string name{};
    AuthSelectorType auth_type{AuthSelectorType::OAuth};
    std::optional<std::string> method_name{std::nullopt};
    std::optional<AuthSelectorStatus> status{std::nullopt};
    bool has_login{false};
};

/// pi `formatAuthSelectorProviderType`: the row label vocabulary.
[[nodiscard]] inline std::string_view format_auth_selector_provider_type(AuthSelectorType type) {
    return type == AuthSelectorType::OAuth ? "subscription" : "API key";
}

using AuthProviderSelectSink =
    std::move_only_function<void(std::string, AuthSelectorType)>;
using AuthProviderCancelSink = std::move_only_function<void()>;

/// The OAuth selector (pi `oauth-selector.ts`): a fuzzy-searchable provider
/// list for the login and logout flows, rendering per-row auth-status
/// indicators and — when the list mixes auth types — `[subscription]` /
/// `[API key]` labels. Selection reports the provider id and auth type;
/// cancellation reports the cancel sink.
///
/// List presentation, the embedded search input, fuzzy ranking and cursor
/// reporting all delegate to a `cch::tui::SelectList` (chrome-framed with
/// the mode title and borders); the component only resolves a selected row
/// back to its `(id, auth type)` pair and keeps the per-mode empty messages.
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
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    AuthSelectorMode mode_;
    std::vector<AuthSelectorProvider> providers_;
    AuthProviderSelectSink on_select_;
    AuthProviderCancelSink on_cancel_;
    cch::tui::SelectList select_list_;
};

} // namespace cch::coding_agent::tui
