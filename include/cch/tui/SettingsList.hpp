#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Style.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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

using SettingsChangeSink = std::move_only_function<void(std::string, std::string)>;
using SettingsCancelSink = std::move_only_function<void()>;
using SettingsSubmenuDoneSink = std::move_only_function<void(std::optional<std::string>)>;
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
};

/// A searchable settings interaction with deterministic cycling and nested selection.
class SettingsList final : public Component, public InputHandler, public Focusable {
public:
    explicit SettingsList(std::vector<SettingItem> items, SettingsListOptions options = {});
    SettingsList(SettingsList&&) noexcept;
    SettingsList& operator=(SettingsList&&) noexcept;
    ~SettingsList() override;

    SettingsList(const SettingsList&) = delete;
    SettingsList& operator=(const SettingsList&) = delete;

    void update_value(std::string id, std::string new_value);
    [[nodiscard]] std::optional<SettingItem> selected_item() const;
    [[nodiscard]] std::string search_query() const;
    [[nodiscard]] bool submenu_open() const;

    [[nodiscard]] util::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    void handle_input(const InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::tui
