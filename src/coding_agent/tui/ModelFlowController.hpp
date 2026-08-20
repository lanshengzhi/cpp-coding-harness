#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cch::ai {
struct Model;
} // namespace cch::ai

namespace cch::coding_agent {
class AgentSession;
class SettingsManager;
} // namespace cch::coding_agent

namespace cch::coding_agent::tui {

class LiveTheme;
class ModalPresenter;
class SharedKeybindings;

/// One immutable model-completion candidate. The `/model` argument
/// completion reads a shared immutable snapshot so the autocomplete request
/// thread never races the executor-confined session state (the snapshot is
/// replaced on the executor whenever the candidate set changes).
struct ModelCompletionItem {
    std::string id;
    std::string provider;
    std::string name;
};
using ModelCompletionSnapshot = std::vector<ModelCompletionItem>;

/// The host seams a ModelFlowController needs beyond presentation (the
/// ModalPresenter) and session state (the AgentSession): executor-confined
/// marshalling, detached coroutine spawning, and the current-session and
/// live-theme lookups. The interactive host wires these to its serialized
/// executor; hooks capture the host weakly so a retired host drops work
/// instead of dangling.
struct ModelFlowHostHooks {
    /// Whether the host is live: running with a composed view (the extracted
    /// flows' pre-extraction `running_ && view_` entry guard).
    std::move_only_function<bool()> is_live{nullptr};
    /// Marshal one input-thread action onto the host executor with the host
    /// liveness gate (the host's post-from-view shape); the action is
    /// dropped once the host stops running.
    std::move_only_function<void(std::move_only_function<void()>)> post_on_executor{nullptr};
    /// Spawn one detached flow coroutine on the host executor; a frame
    /// failure surfaces through the host's failure-label diagnostic.
    std::move_only_function<void(
        std::move_only_function<boost::asio::awaitable<void>()>,
        std::string failure_label)>
        spawn_flow{nullptr};
    /// Resolve the current session at execution time: session replacement
    /// swaps the active session while selector sinks stay open, and the
    /// flows apply to the session that is current when the work runs.
    std::move_only_function<AgentSession*()> current_session{nullptr};
    /// The live component palette, resolved at selector-open time so a theme
    /// change applies to the next selector opened.
    std::move_only_function<const LiveTheme&()> live_theme{nullptr};
};

/// The Native TUI model flows (pi interactive-mode.ts `handleModelCommand`,
/// `showModelSelector`, `showModelsSelector`, `cycleModel`, #503): the
/// `/model [query]` selector flow, the `/models` scoped-models flow, and the
/// Ctrl+P model cycle, extracted from the InteractiveState monolith. The
/// controller owns the coroutine orchestration; it presents strictly through
/// the ModalPresenter seam and reads or mutates session state strictly
/// through the AgentSession resolved from the host hooks, so headless tests
/// can drive the flows against a recording presenter.
///
/// Threading: `open_model_selector`/`open_scoped_models_selector` are
/// any-thread entries that marshal onto the host executor through the hooks;
/// every other method is confined to the host executor. Selector sinks fire
/// on the input thread and re-enter through `post_on_executor`; detached
/// coroutines hold the host lifetime so the presenter, settings, and session
/// referents outlive in-flight flows exactly like the pre-extraction
/// shared-state capture.
class ModelFlowController final : public std::enable_shared_from_this<ModelFlowController> {
public:
    /// `settings_manager` may be null (the scoped-models persist path then
    /// no-ops like the pre-extraction guard); when set it must outlive every
    /// flow. The presenter must outlive every flow; the host enforces this
    /// by tying the presenter's lifetime to `host_lifetime`.
    ModelFlowController(
        boost::asio::any_io_executor executor,
        ModalPresenter& presenter,
        std::weak_ptr<void> host_lifetime,
        ModelFlowHostHooks hooks,
        std::shared_ptr<SharedKeybindings> keybindings,
        coding_agent::SettingsManager* settings_manager);
    ModelFlowController(ModelFlowController&&) = delete;
    ModelFlowController& operator=(ModelFlowController&&) = delete;
    ~ModelFlowController() = default;
    ModelFlowController(const ModelFlowController&) = delete;
    ModelFlowController& operator=(const ModelFlowController&) = delete;

    /// pi `handleModelCommand` entry (`/model [query]`): an empty term opens
    /// the selector; an exact provider/model reference switches immediately;
    /// anything else opens the selector pre-filtered. Any thread.
    void open_model_selector(std::string search_term);
    /// pi `showModelsSelector` entry (`/models`, `/scoped-models`). Any
    /// thread.
    void open_scoped_models_selector();

    /// pi `app.model.select` (Ctrl+L): open the model selector over the
    /// prompt slot. Executor-confined.
    void show_model_selector(std::optional<std::string> initial_search_input);
    /// pi `app.model.cycleForward`/`app.model.cycleBackward` (Ctrl+P /
    /// Shift+Ctrl+P): cycle across the scoped or available models.
    /// Executor-confined.
    void cycle_model(std::string direction);

    /// Rebuild the shared immutable `/model` completion snapshot on the
    /// executor (pi's candidate set: session scoped models when a scope
    /// exists, else the availability snapshot). Called on session bind and
    /// replacement, and by the flows whenever the candidate set changes.
    void update_model_completion();
    /// The current `/model` completion snapshot for the autocomplete
    /// provider (replaced by pointer swap; readers see one consistent list).
    [[nodiscard]] std::shared_ptr<const ModelCompletionSnapshot> model_completion() const;

private:
    [[nodiscard]] boost::asio::awaitable<void> handle_model_command(std::string search_term);
    /// The model selector's select flow: `session.setModel` on the executor
    /// with the `Model: <id>` status (pi `handleSelect`).
    [[nodiscard]] boost::asio::awaitable<void> run_model_switch(cch::ai::Model model);
    [[nodiscard]] boost::asio::awaitable<void> run_model_cycle(std::string direction);
    [[nodiscard]] boost::asio::awaitable<void> run_scoped_models_selector();

    /// pi `updateSessionModels` (executor): session-only scope changes from
    /// the scoped-models selector.
    void apply_scoped_model_change(
        std::optional<std::vector<std::string>> enabled_ids,
        const std::vector<cch::ai::Model>& all_models,
        const std::set<std::string, std::less<>>& all_model_ids);
    /// pi `onPersist` (executor): persist the current selection to the
    /// global `enabledModels` settings field.
    void persist_scoped_models(
        std::optional<std::vector<std::string>> enabled_ids,
        const std::vector<cch::ai::Model>& all_models,
        const std::set<std::string, std::less<>>& all_model_ids);

    /// Spawn one detached flow through the host hook; the frame holds the
    /// controller and the host lifetime for the coroutine's whole life.
    void spawn(
        std::move_only_function<boost::asio::awaitable<void>()> start,
        std::string failure_label);

    boost::asio::any_io_executor executor_;
    ModalPresenter* presenter_; // kept alive by host_lifetime_ across in-flight flows.
    std::weak_ptr<void> host_lifetime_;
    ModelFlowHostHooks hooks_;
    std::shared_ptr<SharedKeybindings> keybindings_;
    coding_agent::SettingsManager* settings_manager_; // may be null; must outlive every flow.
    /// The `/model` completion snapshot, replaced only by
    /// `update_model_completion()` on the executor.
    std::shared_ptr<const ModelCompletionSnapshot> model_completion_{};
};

} // namespace cch::coding_agent::tui
