#include "ThemeCatalog.hpp"

#include "coding_agent/ResourceDiagnosticPolicy.hpp"

#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] util::Error selection_error(std::string message, std::string detail = {}) {
    cch::coding_agent::detail::bound_resource_diagnostic_text(message);
    cch::coding_agent::detail::bound_resource_diagnostic_text(detail);
    return util::make_error(
        util::ErrorCode::Validation,
        std::move(message),
        std::move(detail));
}

struct ThemeSettingsState {
    ThemeController* controller; // must outlive the settings overlay.
    std::optional<util::Error> error{std::nullopt};
};

class ThemeSettingsList final : public cch::tui::Component,
                                public cch::tui::InputHandler,
                                public cch::tui::Focusable {
public:
    ThemeSettingsList(ThemeController& controller, ThemeSettingsCancelSink on_cancel)
        : state_(std::make_shared<ThemeSettingsState>(ThemeSettingsState{.controller = &controller})),
          list_(make_items(controller), make_options(controller, state_, std::move(on_cancel))) {}

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        if (state_->error) return std::unexpected(*state_->error);
        return list_.render(width);
    }

    void invalidate() override {
        list_.invalidate();
    }

    void handle_input(const cch::tui::InputEventVariant& input) override {
        list_.handle_input(input);
    }

    [[nodiscard]] bool accepts_key_releases() const override {
        return list_.accepts_key_releases();
    }

    void set_focused(bool focused) override {
        list_.set_focused(focused);
    }

    [[nodiscard]] bool focused() const override {
        return list_.focused();
    }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return list_.cursor_location();
    }

private:
    [[nodiscard]] static std::vector<cch::tui::SettingItem> make_items(ThemeController& controller) {
        return {{
            .id = "theme",
            .label = "Theme",
            .description = "Select the active Native TUI theme",
            .current_value = std::string(controller.active_theme_name()),
            .control = cch::tui::SettingSubmenu{},
        }};
    }

    [[nodiscard]] static cch::tui::SettingsListOptions make_options(
        ThemeController& controller,
        const std::shared_ptr<ThemeSettingsState>& state,
        ThemeSettingsCancelSink on_cancel) {
        auto cancel = std::make_shared<ThemeSettingsCancelSink>(std::move(on_cancel));
        return {
            .max_visible = 5,
            .enable_search = false,
            .theme = controller.live_theme().settings_list_theme(),
            .on_cancel = [cancel]() {
                if (*cancel) (*cancel)();
            },
            .submenu_factory = [state](
                                   const cch::tui::SettingItem&,
                                   cch::tui::SettingsSubmenuDoneSink done) {
                std::vector<cch::tui::SelectItem> items;
                const auto names = state->controller->available_theme_names();
                items.reserve(names.size());
                std::size_t selected_index = 0;
                for (std::size_t index = 0; index < names.size(); ++index) {
                    if (names[index] == state->controller->active_theme_name()) selected_index = index;
                    items.push_back({
                        .value = names[index],
                        .label = names[index],
                        .description = names[index] == state->controller->active_theme_name()
                            ? std::optional<std::string>{"(current)"}
                            : std::nullopt,
                    });
                }

                auto completion = std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done));
                auto list = std::make_unique<cch::tui::SelectList>(
                    std::move(items),
                    cch::tui::SelectListOptions{
                        .max_visible = 10,
                        .theme = state->controller->live_theme().select_list_theme(),
                        .on_select = [state, completion](const cch::tui::SelectItem& item) {
                            if (auto selected = state->controller->select_theme(item.value); !selected) {
                                state->error = selected.error();
                                (*completion)(std::nullopt);
                            } else {
                                (*completion)(item.value);
                            }
                        },
                        .on_cancel = [completion]() { (*completion)(std::nullopt); },
                    });
                list->set_selected_index(selected_index);
                return list;
            },
        };
    }

    std::shared_ptr<ThemeSettingsState> state_;
    cch::tui::SettingsList list_;
};

} // namespace

struct ThemeController::Impl {
    Impl(
        ThemeCatalogResult catalog,
        cch::tui::Tui& configured_root,
        cch::tui::TerminalColorCapability configured_capability,
        ThemeSelectionCommitter configured_committer)
        : resources(std::move(catalog.effective_themes)),
          active_name(std::move(catalog.initial_theme_name)),
          active_origin(catalog.initial_theme_origin),
          live(std::move(catalog.initial_theme), configured_capability),
          root(configured_root),
          capability(configured_capability),
          committer(std::move(configured_committer)) {}

    std::vector<ThemeResource> resources;
    std::string active_name;
    ThemeResourceOrigin active_origin{ThemeResourceOrigin::Builtin};
    LiveTheme live;
    cch::tui::Tui& root; // must outlive this controller.
    cch::tui::TerminalColorCapability capability{cch::tui::TerminalColorCapability::Xterm256};
    ThemeSelectionCommitter committer;
};

ThemeController::ThemeController(
    ThemeCatalogResult catalog,
    cch::tui::Tui& root,
    cch::tui::TerminalColorCapability color_capability,
    ThemeSelectionCommitter committer)
    : impl_(std::make_unique<Impl>(
          std::move(catalog),
          root,
          color_capability,
          std::move(committer))) {}

ThemeController::~ThemeController() = default;

std::string_view ThemeController::active_theme_name() const {
    return impl_->active_name;
}

ThemeResourceOrigin ThemeController::active_theme_origin() const {
    return impl_->active_origin;
}

std::vector<std::string> ThemeController::available_theme_names() const {
    std::vector<std::string> names;
    names.reserve(impl_->resources.size());
    for (const auto& resource : impl_->resources) names.push_back(resource.theme.name);
    return names;
}

const LiveTheme& ThemeController::live_theme() const {
    return impl_->live;
}

util::ExpectedVoid ThemeController::select_theme(std::string_view name) {
    const auto found = std::find_if(
        impl_->resources.begin(),
        impl_->resources.end(),
        [name](const auto& resource) { return resource.theme.name == name; });
    if (found == impl_->resources.end()) {
        return std::unexpected(selection_error(
            "theme selection is unavailable",
            std::string(name)));
    }
    if (impl_->committer) {
        try {
            if (auto committed = impl_->committer(name); !committed) {
                return std::unexpected(committed.error());
            }
        } catch (const std::exception& error) {
            return std::unexpected(selection_error(
                "theme selection committer failed",
                error.what()));
        } catch (...) {
            return std::unexpected(selection_error("theme selection committer failed"));
        }
    }

    impl_->live.replace(found->theme, impl_->capability);
    impl_->active_name = found->theme.name;
    impl_->active_origin = found->origin;
    impl_->root.invalidate();
    return {};
}

util::Expected<std::unique_ptr<cch::tui::Overlay>> make_theme_settings_overlay(
    ThemeController& controller,
    ThemeSettingsCancelSink on_cancel) {
    cch::tui::OverlayOptions options;
    options.position = cch::tui::OverlayPosition::TopLeft;
    options.size_constraints.max_width = 50;
    options.size_constraints.max_height = 18;
    options.z_index = 100;
    auto overlay = std::make_unique<cch::tui::Overlay>(std::move(options));
    auto settings = std::make_unique<ThemeSettingsList>(controller, std::move(on_cancel));
    if (auto attached = overlay->add_child(std::move(settings)); !attached) {
        return std::unexpected(attached.error());
    }
    return overlay;
}

} // namespace cch::coding_agent::tui
