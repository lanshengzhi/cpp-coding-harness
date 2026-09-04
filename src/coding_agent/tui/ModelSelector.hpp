#pragma once

#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

using ModelSelectorSelectSink = std::move_only_function<void(ai::Model)>;
using ModelSelectorCancelSink = std::move_only_function<void()>;
using ModelSelectorInvalidateSink = std::move_only_function<void()>;

/// The model selector (pi `model-selector.ts`): replaces the editor during
/// `app.model.select` (Ctrl+L) and renders the model list with fuzzy search
/// and the all/scoped scope toggle. The list renders the current availability
/// snapshot immediately, then refreshes availability in the background
/// (pi `refreshModels`); the subset's availability refresh is local and
/// side-effect-free, so pi's 15s timeout path is unreachable and omitted, and
/// a refresh failure collapses to one generic message (the C++ runtime
/// exposes a single composition-diagnostics channel rather than pi's
/// per-provider error counts).
///
/// The list rows and the search input are delegated to the shared
/// `cch::tui::SelectList` (search enabled); the component owns the chrome
/// around it (border, scope/warning lines, status/diagnostics rows) because
/// the refresh status appears and disappears as the background refresh
/// settles — too dynamic for SelectList's construction-time title/hint. The
/// scope toggle (Tab) is pre-dispatched here so the scope marker text can
/// re-accent each render; everything else flows into SelectList (navigation,
/// confirm/cancel, search editing).
///
/// Selecting a model fires `on_select` (pi's `onSelect`); the settings
/// default write pi performs in the selector (`setDefaultModelAndProvider`)
/// rides the session `setModel` path in this subset, which persists the same
/// global default.
///
/// Threading: input handling and render run on the TUI thread; the background
/// refresh runs on the injected executor. A mutex serializes the model lists
/// and status state; the `SelectList` (and its embedded search Input) is only
/// touched from the TUI thread, and its item set is refreshed at render time
/// whenever the mutex-guarded revision advanced (refresh result or scope
/// toggle), so the query and selection survive rebuilds.
class ModelSelectorComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable,
      public std::enable_shared_from_this<ModelSelectorComponent> {
public:
    ModelSelectorComponent(
        const LiveTheme& theme,
        std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
        const ai::Model* current_model,
        std::shared_ptr<cch::coding_agent::ModelRuntime> runtime,
        boost::asio::any_io_executor executor,
        std::vector<cch::coding_agent::ScopedModel> scoped_models,
        ModelSelectorSelectSink on_select,
        ModelSelectorCancelSink on_cancel,
        ModelSelectorInvalidateSink on_invalidate,
        std::optional<std::string> initial_search_input = std::nullopt);
    ModelSelectorComponent(ModelSelectorComponent&&) = delete;
    ModelSelectorComponent& operator=(ModelSelectorComponent&&) = delete;
    ~ModelSelectorComponent() override;
    ModelSelectorComponent(const ModelSelectorComponent&) = delete;
    ModelSelectorComponent& operator=(const ModelSelectorComponent&) = delete;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override {}
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    /// The search input's cursor translated into this component's own line
    /// coordinates: SelectList reports the row of its search line, and the
    /// rows this component emits above the SelectList (border, spacer,
    /// scope/warning block, spacer) are added on top. Reported only when
    /// focused, rendered, and the SelectList itself reports a cursor.
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    struct ModelItem {
        std::string provider;
        std::string id;
        ai::Model model;
    };

    void load_models_from_snapshot();
    void start_refresh();
    /// Mark the selector closed so an in-flight refresh stops mutating state.
    /// Callers hold the mutex.
    void close();
    void set_scope(bool scoped);
    /// The current scope's models as SelectList items: value is the unique
    /// `provider/id` reference, the label is the `id [provider]` row with the
    /// current-model marker, and the search text is the pi
    /// `getModelSelectorSearchText` text so ranking matches the hand-rolled
    /// fuzzy filter it replaces. Callers hold the mutex (or run in the ctor).
    [[nodiscard]] std::vector<cch::tui::SelectItem> build_select_items() const;
    void confirm_selection(const cch::tui::SelectItem& item);
    void cancel_selection();
    [[nodiscard]] bool is_current(const ai::Model& model) const;
    [[nodiscard]] std::string scope_text() const;
    [[nodiscard]] std::string scope_hint_text() const;

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    std::shared_ptr<cch::coding_agent::ModelRuntime> runtime_;
    boost::asio::any_io_executor executor_;
    ModelSelectorSelectSink on_select_;
    ModelSelectorCancelSink on_cancel_;
    ModelSelectorInvalidateSink on_invalidate_;
    cch::tui::SelectList select_list_;

    mutable std::mutex mutex_;
    std::optional<ai::Model> current_model_;
    std::vector<ModelItem> all_models_;
    std::vector<ModelItem> scoped_model_items_;
    std::vector<ModelItem> active_models_;
    bool scope_scoped_{false};
    std::optional<std::string> error_message_{std::nullopt};
    std::string refresh_status_message_{"Refreshing model catalogs…"};
    bool refresh_status_success_{false};
    /// Bumped whenever the active item set changes (snapshot load or scope
    /// toggle) so render can push fresh items into the SelectList exactly
    /// once per change, preserving the query and selection.
    std::size_t items_revision_{0};
    std::size_t applied_items_revision_{0};
    /// Rows this component emitted above the SelectList in the last
    /// successful render; cursor_location adds it to SelectList's row.
    std::size_t cursor_row_offset_{0};
    /// Set on the first render to start the one-shot background refresh.
    bool refresh_started_{false};
    bool closed_{false};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
