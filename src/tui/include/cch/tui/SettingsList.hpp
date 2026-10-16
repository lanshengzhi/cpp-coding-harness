#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Mouse.hpp>
#include <cch/tui/Style.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cch::tui {

struct SettingValues {
    std::vector<std::string> values{};

    bool operator==(const SettingValues&) const = default;
};

struct SettingSubmenu {
    bool operator==(const SettingSubmenu&) const = default;
};

using SettingControlVariant = std::variant<std::monostate, SettingValues, SettingSubmenu>;

struct SettingItem {
    std::string id{};
    std::string label{};
    std::optional<std::string> description{std::nullopt};
    std::string current_value{};
    SettingControlVariant control{std::monostate{}};

    bool operator==(const SettingItem&) const = default;
};

using SettingsChangeSink = std::move_only_function<support::ExpectedVoid(std::string, std::string)>;
using SettingsCancelSink = std::move_only_function<support::ExpectedVoid()>;

struct SettingsSubmenuOptions {
    std::optional<std::string> navigate_to{std::nullopt};
};

using SettingsSubmenuDoneSink = std::move_only_function<support::ExpectedVoid(
        std::optional<std::string>, std::optional<SettingsSubmenuOptions>)>;
using SettingsSubmenuFactoryHook = std::move_only_function<std::unique_ptr<Component>(
    const SettingItem&,
    SettingsSubmenuDoneSink)>;

struct SettingsListTheme {
    SelectionStyleHook label{};
    SelectionStyleHook value{};
    TextStyleHook description{};
    std::string cursor{"→ "};
    TextStyleHook hint{};
};

struct SettingsListOptions {
    std::size_t max_visible{5};
    bool enable_search{false};
    SettingsListTheme theme{};
    SettingsChangeSink on_change{};
    SettingsCancelSink on_cancel{};
    SettingsSubmenuFactoryHook submenu_factory{};
    std::shared_ptr<const KeybindingRegistry> keybindings{};
    std::shared_ptr<KeybindingsContext> keybinding_context{};
};

/// A searchable settings interaction with deterministic cycling and nested selection.
///
/// When search is enabled, the search line is a single-line `Input` component
/// (pi `components/settings-list.ts`): editing behaviors and paste cleaning are
/// the component's, ranking is the public `Fuzzy` module's, and space confirms
/// only while the search is empty.
class SettingsList final : public Component, public InputHandler, public Focusable, public MouseHandler {
public:
    explicit SettingsList(std::vector<SettingItem> items, SettingsListOptions options = {});
    SettingsList(SettingsList&&) noexcept;
    SettingsList& operator=(SettingsList&&) noexcept;
    ~SettingsList() override;

    SettingsList(const SettingsList&) = delete;
    SettingsList& operator=(const SettingsList&) = delete;

    void update_value(std::string id, std::string new_value);
    void select_item(std::string_view id);
    [[nodiscard]] std::optional<SettingItem> selected_item() const;
    [[nodiscard]] std::string search_query() const;
    [[nodiscard]] bool submenu_open() const;

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    InputAdmissionOutcome handle_input(const InputEventVariant& input) override;
    /// A left press selects the rendered row and a left click activates it; the
    /// wheel moves the selection by one row per notch; the search row forwards
    /// to the search `Input` (pi `SettingsList.handleMouse`).
    [[nodiscard]] std::optional<MouseEventResult> handle_mouse(const MouseEvent& event) override;
    /// An open submenu owns every mouse event: its resolved dispatch result is
    /// forwarded unchanged so the submenu keeps the target while this list
    /// keeps keyboard focus (pi `SettingsList.handleMouse` submenu branch).
    [[nodiscard]] std::optional<MouseDispatchResult> handle_mouse_forwarded(const MouseEvent& event) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
