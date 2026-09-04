#pragma once

#include <cch/ai/Model.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
///
/// List presentation, fuzzy filtering, the embedded search input and the
/// cursor delegate to a `cch::tui::SelectList`; the component keeps the
/// title/session-only/footer chrome and intercepts the `app.models.*`
/// domain actions (toggle-on-confirm, filtered enable-all/clear-all,
/// provider toggling, reorder, save, Ctrl+C-clears-search-first) before the
/// SelectList sees those keys.
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
    void invalidate() override;
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    [[nodiscard]] bool is_enabled(std::string_view id) const;
    void toggle(std::string id);
    void enable_all(std::optional<std::vector<std::string>> target_ids);
    void clear_all(std::optional<std::vector<std::string>> target_ids);
    void toggle_provider(std::string_view provider);
    void move(std::string_view id, int delta);
    /// Re-push the current domain state into the SelectList, re-applying its
    /// live query and restoring the selection by item value.
    void sync_items();
    void notify_change();
    [[nodiscard]] std::string footer_text() const;

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    ScopedModelsChangeSink on_change_;
    ScopedModelsPersistSink on_persist_;
    ScopedModelsCancelSink on_cancel_;

    // pi `modelsById` / `allIds`: model lookups plus the provider order.
    std::vector<std::pair<std::string, ai::Model>> models_by_id_;
    std::vector<std::string> all_ids_;
    /// pi `enabledIds`: null = all enabled; otherwise the explicit ordered
    /// list of full ids.
    std::optional<std::vector<std::string>> enabled_ids_{std::nullopt};
    /// The currently filtered ids in display order, captured by the search
    /// filter hook on every ranking pass: the target set of the filtered
    /// enable-all/clear-all actions.
    std::vector<std::string> filtered_ids_;
    cch::tui::SelectList select_list_;
    bool dirty_{false};
    /// Component chrome rows emitted above the SelectList at the last render;
    /// the SelectList reports its search row relative to its own output, so
    /// this offset moves the cursor onto the real search line.
    std::size_t rows_before_list_{0};
};

} // namespace cch::coding_agent::tui
