#pragma once

#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include "coding_agent/tui/SearchableSelectList.hpp"
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;

using ModelSelectorSelectSink = std::move_only_function<void(ai::Model)>;
using ModelSelectorSelectAsDefaultSink = std::move_only_function<void(ai::Model)>;
using ModelSelectorCancelSink = std::move_only_function<void()>;
using ModelSelectorInvalidateSink = std::move_only_function<void()>;

/// The model selector (pi `model-selector.ts`): replaces the editor during
/// `app.model.select` (Ctrl+L) and renders the model list with fuzzy search
/// and the all/scoped scope toggle. The list renders the current gateway
/// catalog immediately, then refreshes the live Magpie catalog in the
/// background (`refresh_async`); closing the selector requests the refresh's
/// cancellation token, so a departed selector neither blocks terminal
/// teardown nor mutates the list after close. A failed refresh clears the
/// active catalog and surfaces the typed discovery error; a cancelled
/// refresh preserves it.
///
/// The list rows are delegated to `SearchableSelectList`, which owns the
/// search Input and passes filtered rows into the toolkit SelectList. This
/// component owns the chrome (border, scope/warning, and status rows) because
/// the refresh status appears and disappears as the background refresh
/// settles. The scope toggle (Tab) is pre-dispatched here so the scope
/// marker text can re-accent each render; navigation, confirm/cancel, and
/// search editing flow through SearchableSelectList.
///
/// Selecting a model fires `on_select` (pi's `onSelect`); the save-as-default
/// key (`app.models.save`, Ctrl+S) fires `on_select_as_default` (pi's
/// `onSelectAsDefault`) and the dim save hint renders only when that sink is
/// present (pi's constructor-gated hint line). The session-only vs persisted
/// distinction and the pi statuses (`Model: <id>` vs `Default model:
/// provider/id`) ride the flow controller's mutation options (#774).
///
/// Threading: input handling and render run on the TUI thread; the background
/// refresh runs on the injected executor. A mutex serializes the model lists
/// and status state; the `SearchableSelectList` and its toolkit list are only
/// touched from the TUI thread, and its item set is refreshed at render time
/// whenever the mutex-guarded revision advanced (refresh result or scope
/// toggle), so the query and selection survive rebuilds.
class ModelSelectorComponent final
    : public cch::tui::Component,
      public cch::tui::InputHandler,
      public cch::tui::Focusable,
      public std::enable_shared_from_this<ModelSelectorComponent> {
public:
    ModelSelectorComponent(const LiveTheme& theme,
            std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings,
            const ai::Model* current_model,
            std::shared_ptr<cch::coding_agent::ModelRuntime> runtime,
            boost::asio::any_io_executor executor,
            std::vector<cch::coding_agent::ScopedModel> scoped_models,
            ModelSelectorSelectSink on_select,
            ModelSelectorCancelSink on_cancel,
            ModelSelectorInvalidateSink on_invalidate,
            std::optional<std::string> initial_search_input = std::nullopt,
            ModelSelectorSelectAsDefaultSink on_select_as_default = {});
    ModelSelectorComponent(ModelSelectorComponent&&) = delete;
    ModelSelectorComponent& operator=(ModelSelectorComponent&&) = delete;
    ~ModelSelectorComponent() override;
    ModelSelectorComponent(const ModelSelectorComponent&) = delete;
    ModelSelectorComponent& operator=(const ModelSelectorComponent&) = delete;

    /// Mark the selector closed (no further refresh mutation) and request the
    /// cancellation of an in-flight catalog refresh. Idempotent; called by the
    /// destructor and by the engine when the prompt slot is replaced.
    void close() noexcept;

    [[nodiscard]] support::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override {}
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    /// The search input's cursor translated into this component's own line
    /// coordinates: SearchableSelectList reports the search cursor, and the
    /// rows this component emits above the list (border, spacer,
    /// scope/warning block, spacer) are added on top. Reported only when
    /// focused and rendered.
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override;

private:
    struct ModelItem {
        std::string provider;
        std::string id;
        ai::Model model;
    };

    void load_models_from_snapshot();
    void start_refresh();
    void set_scope(bool scoped);
    /// The current scope's models as SelectList items: value is the unique
    /// `provider/id` reference, the label is the `✓ id [provider]` row whose
    /// current-model marker leads the label, and the search text is the pi
    /// `getModelSelectorSearchText` text so ranking matches the hand-rolled
    /// fuzzy filter it replaces. Callers hold the mutex (or run in the ctor).
    [[nodiscard]] std::vector<cch::tui::SelectItem> build_select_items() const;
    void confirm_selection(const cch::tui::SelectItem& item);
    void confirm_selection_as_default(const cch::tui::SelectItem& item);
    void cancel_selection();
    [[nodiscard]] bool is_current(const ai::Model& model) const;
    [[nodiscard]] std::string scope_text() const;
    [[nodiscard]] std::string scope_hint_text() const;

    const LiveTheme& theme_; // must outlive this component.
    std::shared_ptr<const cch::tui::KeybindingRegistry> keybindings_;
    std::shared_ptr<cch::coding_agent::ModelRuntime> runtime_;
    boost::asio::any_io_executor executor_;
    ModelSelectorSelectSink on_select_;
    ModelSelectorSelectAsDefaultSink on_select_as_default_;
    ModelSelectorCancelSink on_cancel_;
    ModelSelectorInvalidateSink on_invalidate_;
    SearchableSelectList select_list_;

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
    /// toggle) so render can push fresh items into SearchableSelectList exactly
    /// once per change, preserving the query and selection.
    std::size_t items_revision_{0};
    std::size_t applied_items_revision_{0};
    /// Rows this component emitted above the list in the last successful
    /// render; cursor_location adds the search cursor's relative row.
    std::size_t cursor_row_offset_{0};
    /// Set on the first render to start the one-shot background refresh.
    bool refresh_started_{false};
    /// Cancels the in-flight catalog request when the selector closes.
    std::stop_source refresh_stop_source_;
    std::atomic<bool> closed_{false};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
