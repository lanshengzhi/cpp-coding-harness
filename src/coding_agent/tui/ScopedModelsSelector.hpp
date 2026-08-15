#pragma once

#include <cch/ai/Model.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

/// Fired whenever the enabled set or order changes (session-only, no persist).
/// `std::nullopt` means all enabled (pi's `null`), a vector the explicit
/// ordered list of `provider/id` entries.
using ScopedModelsChangeSink =
    std::move_only_function<void(std::optional<std::vector<std::string>> /*enabled_ids*/)>;
/// Fired when the user persists the current selection to settings (pi
/// `onPersist`); same null semantics as the change sink.
using ScopedModelsPersistSink =
    std::move_only_function<void(std::optional<std::vector<std::string>> /*enabled_ids*/)>;
using ScopedModelsCancelSink = std::move_only_function<void()>;

/// The scoped-models selector (pi `scoped-models-selector.ts`): enables,
/// disables, and reorders the models Ctrl+P cycles through, with the six
/// `app.models.*` actions bound inside it. Changes are session-only until
/// explicitly saved with `app.models.save` (Ctrl+S), which fires `on_persist`.
///
/// `enabled_ids`: `std::nullopt` means all enabled (no filter); a vector is
/// the explicit ordered list of `provider/id` entries (entries whose model is
/// unavailable are retained and rendered `[unavailable]`, pi's
/// `getSortedIds`). Sinks fire on the input thread; hosts post to their
/// executor like the login presentation sinks.
class ScopedModelsSelectorComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable {
public:
    ScopedModelsSelectorComponent(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        std::vector<ai::Model> all_models,
        std::optional<std::vector<std::string>> enabled_ids,
        ScopedModelsChangeSink on_change,
        ScopedModelsPersistSink on_persist,
        ScopedModelsCancelSink on_cancel);
    ScopedModelsSelectorComponent(ScopedModelsSelectorComponent&&) = delete;
    ScopedModelsSelectorComponent& operator=(ScopedModelsSelectorComponent&&) = delete;
    ~ScopedModelsSelectorComponent() override = default;
    ScopedModelsSelectorComponent(const ScopedModelsSelectorComponent&) = delete;
    ScopedModelsSelectorComponent& operator=(const ScopedModelsSelectorComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override {}
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    struct ModelItem {
        std::string full_id;
        std::optional<ai::Model> model;
        bool enabled{false};
    };

    [[nodiscard]] bool is_enabled(std::string_view id) const;
    void toggle(std::string id);
    void enable_all(std::optional<std::vector<std::string>> target_ids);
    void clear_all(std::optional<std::vector<std::string>> target_ids);
    void toggle_provider(std::string_view provider);
    void move(std::string_view id, int delta);
    void refresh();
    void notify_change();
    [[nodiscard]] std::vector<ModelItem> build_items() const;
    [[nodiscard]] std::string footer_text() const;
    [[nodiscard]] support::ExpectedVoid update_list(std::vector<std::string>& out_lines, std::size_t width) const;

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    ScopedModelsChangeSink on_change_;
    ScopedModelsPersistSink on_persist_;
    ScopedModelsCancelSink on_cancel_;
    cch::tui::Input search_input_;

    // pi `modelsById` / `allIds`: model lookups plus the provider order.
    std::vector<std::pair<std::string, ai::Model>> models_by_id_;
    std::vector<std::string> all_ids_;
    /// pi `enabledIds`: null = all enabled; otherwise the explicit ordered
    /// list of full ids.
    std::optional<std::vector<std::string>> enabled_ids_{std::nullopt};
    std::vector<ModelItem> filtered_items_;
    std::size_t selected_index_{0};
    bool dirty_{false};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
