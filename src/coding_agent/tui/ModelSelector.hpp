#pragma once

#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/util/Error.hpp>

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
/// Selecting a model fires `on_select` (pi's `onSelect`); the settings
/// default write pi performs in the selector (`setDefaultModelAndProvider`)
/// rides the session `setModel` path in this subset, which persists the same
/// global default.
///
/// Threading: input handling and render run on the TUI thread; the background
/// refresh runs on the injected executor. A mutex serializes the model lists,
/// search query, and status state; the search Input itself is only touched
/// from the TUI thread (pi's `searchInput`).
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

    [[nodiscard]] util::Expected<cch::tui::RenderResult> render(std::size_t width) override;
    void invalidate() override {}
    void handle_input(const cch::tui::InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override { return false; }
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
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
    void filter_models(std::string query);
    void update_list(std::vector<std::string>& out_lines) const;
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
    cch::tui::Input search_input_;

    mutable std::mutex mutex_;
    std::optional<ai::Model> current_model_;
    std::vector<ModelItem> all_models_;
    std::vector<ModelItem> scoped_model_items_;
    std::vector<ModelItem> active_models_;
    std::vector<ModelItem> filtered_models_;
    std::size_t selected_index_{0};
    bool scope_scoped_{false};
    std::string search_query_{};
    std::optional<std::string> error_message_{std::nullopt};
    std::string refresh_status_message_{"Refreshing model catalogs…"};
    bool refresh_status_success_{false};
    /// Set on the first render to start the one-shot background refresh.
    bool refresh_started_{false};
    bool closed_{false};
    bool focused_{false};
};

} // namespace cch::coding_agent::tui
