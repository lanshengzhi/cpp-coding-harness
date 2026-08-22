#include "ModelFlowController.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/ModelSelector.hpp"
#include "coding_agent/tui/ScopedModelsSelector.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"

#include <cch/ai/Model.hpp>
#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/Error.hpp>
#include "ai/AsyncResultBridge.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

/// pi `showModelsSelector` initial enabled ids: the session scope when one
/// exists, else the configured scope's resolved ids, with `no-match` pattern
/// ids appended as unavailable entries (pi's `currentEnabledIds` assembly).
[[nodiscard]] std::optional<std::vector<std::string>> initial_selector_enabled_ids(
    const std::vector<ScopedModel>& session_scoped_models,
    const std::optional<ModelScopeResolution>& configured_scope) {
    std::optional<std::vector<std::string>> ids;
    if (!session_scoped_models.empty()) {
        ids = std::vector<std::string>{};
        for (const auto& entry : session_scoped_models) {
            ids->push_back(entry.model.provider + "/" + entry.model.id);
        }
    } else if (configured_scope) {
        ids = std::vector<std::string>{};
        for (const auto& scoped : configured_scope->scoped_models) {
            ids->push_back(scoped.model.provider + "/" + scoped.model.id);
        }
    }
    for (const auto& diagnostic : configured_scope ? configured_scope->diagnostics
                                                   : std::vector<ModelScopeDiagnostic>{}) {
        if (diagnostic.code != "no-match") continue;
        if (!ids) ids = std::vector<std::string>{};
        if (std::find(ids->begin(), ids->end(), diagnostic.pattern) == ids->end()) {
            ids->push_back(diagnostic.pattern);
        }
    }
    return ids;
}

/// Resolve the host's current session; null when the host wires no session
/// (unbound or retired host).
[[nodiscard]] coding_agent::AgentSession* current_session(ModelFlowHostHooks& hooks) {
    return hooks.current_session != nullptr ? hooks.current_session() : nullptr;
}

} // namespace

ModelFlowController::ModelFlowController(
    boost::asio::any_io_executor executor,
    ModalPresenter& presenter,
    std::weak_ptr<void> host_lifetime,
    ModelFlowHostHooks hooks,
    std::shared_ptr<SharedKeybindings> keybindings,
    coding_agent::SettingsManager* settings_manager)
    : executor_(std::move(executor)),
      presenter_(&presenter),
      host_lifetime_(std::move(host_lifetime)),
      hooks_(std::move(hooks)),
      keybindings_(std::move(keybindings)),
      settings_manager_(settings_manager) {}

// ── Entries ──────────────────────────────────────────────────────────────

/// pi `handleModelCommand`: the `/model [query]` entry posts one flow to the
/// executor with the host liveness gate (the pre-extraction
/// `post_open_model_selector` shape).
void ModelFlowController::open_model_selector(std::string search_term) {
    if (hooks_.post_on_executor == nullptr) return;
    auto self = shared_from_this();
    hooks_.post_on_executor([self, search_term = std::move(search_term)]() mutable {
        self->spawn(
            [self, search_term = std::move(search_term)]() mutable -> boost::asio::awaitable<void> {
                co_await self->handle_model_command(std::move(search_term));
            },
            "Native TUI model command failed");
    });
}

/// pi `showModelsSelector`: the `/models` (`/scoped-models`) entry posts one
/// flow to the executor with the host liveness gate.
void ModelFlowController::open_scoped_models_selector() {
    if (hooks_.post_on_executor == nullptr) return;
    auto self = shared_from_this();
    hooks_.post_on_executor([self]() mutable {
        self->spawn(
            [self]() -> boost::asio::awaitable<void> {
                co_await self->run_scoped_models_selector();
            },
            "Native TUI scoped-models selector failed");
    });
}

/// pi `app.model.cycleForward`/`app.model.cycleBackward`: one detached cycle
/// flow on the executor (the pre-extraction `dispatch(CycleModelAction)`
/// shape).
void ModelFlowController::cycle_model(std::string direction) {
    auto self = shared_from_this();
    spawn(
        [self, direction = std::move(direction)]() mutable -> boost::asio::awaitable<void> {
            co_await self->run_model_cycle(std::move(direction));
        },
        "Native TUI model cycle failed");
}

// ── Model completion snapshot ────────────────────────────────────────────

/// Rebuild the shared immutable `/model` completion snapshot on the executor
/// (pi's candidate set: session scoped models when a scope exists, else the
/// availability snapshot). The snapshot is only ever replaced here, so
/// autocomplete readers see one consistent list.
void ModelFlowController::update_model_completion() {
    auto* session = current_session(hooks_);
    auto runtime = session != nullptr ? session->model_runtime() : nullptr;
    if (!runtime) {
        model_completion_ = std::make_shared<const ModelCompletionSnapshot>();
        return;
    }
    auto snapshot = std::make_shared<ModelCompletionSnapshot>();
    const auto scoped = session->scoped_models();
    if (!scoped.empty()) {
        snapshot->reserve(scoped.size());
        for (const auto& entry : scoped) {
            snapshot->push_back(ModelCompletionItem{
                .id = entry.model.id,
                .provider = entry.model.provider,
                .name = entry.model.name,
            });
        }
    } else {
        for (const auto& model : runtime->get_available_snapshot()) {
            snapshot->push_back(ModelCompletionItem{
                .id = model.id,
                .provider = model.provider,
                .name = model.name,
            });
        }
    }
    model_completion_ = std::move(snapshot);
}

std::shared_ptr<const ModelCompletionSnapshot> ModelFlowController::model_completion() const {
    return model_completion_;
}

// ── Model selector (pi `showModelSelector`/`handleModelCommand`) ─────────

/// pi `showModelSelector`: the model selector renders in the editor slot
/// (pi's `showSelector` editorContainer swap). Selecting a model runs
/// `session.setModel` on the executor and reports `Model: <id>`; the
/// settings default write rides the session path.
void ModelFlowController::show_model_selector(std::optional<std::string> initial_search_input) {
    if (hooks_.is_live == nullptr || !hooks_.is_live()) return;
    if (hooks_.live_theme == nullptr) return;
    auto* session = current_session(hooks_);
    if (session == nullptr || !session->is_open()) return;
    const auto current_model = session->snapshot().agent_state.model;
    const auto weak = weak_from_this();
    auto selector = std::make_shared<ModelSelectorComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        &current_model,
        session->model_runtime(),
        executor_,
        session->scoped_models(),
        [weak](cch::ai::Model model) {
            // Input-thread sink: post the session switch to the executor.
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor([self, model = std::move(model)]() mutable {
                    self->spawn(
                        [self, model = std::move(model)]() mutable -> boost::asio::awaitable<void> {
                            co_await self->run_model_switch(std::move(model));
                        },
                        "Native TUI model selection failed");
                });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor([self] { self->presenter_->restore_prompt_slot(); });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) self->presenter_->request_render();
        },
        std::move(initial_search_input));
    presenter_->replace_prompt_slot(std::move(selector));
}

/// pi `handleModelCommand`: no search term opens the selector; an exact
/// provider/model reference switches immediately (`Model: <id>`); anything
/// else opens the selector pre-filtered with the term.
boost::asio::awaitable<void> ModelFlowController::handle_model_command(std::string search_term) {
    const auto term = interactive_view_detail::trim_editor_submission(std::move(search_term));
    if (term.empty()) {
        show_model_selector(std::nullopt);
        co_return;
    }
    auto* session = current_session(hooks_);
    if (session == nullptr) co_return;

    // pi `getModelCandidates`: the scoped set when present, else a live
    // availability refresh.
    std::vector<cch::ai::Model> candidates;
    const auto scoped = session->scoped_models();
    if (!scoped.empty()) {
        candidates.reserve(scoped.size());
        for (const auto& entry : scoped) candidates.push_back(entry.model);
    } else {
        auto runtime = session->model_runtime();
        if (!runtime) co_return;
        (void)runtime->refresh();
        auto available = co_await ai::detail::await_async_result(runtime->get_available());
        if (!available) {
            presenter_->show_error(combined_error_text(available.error()));
            co_return;
        }
        candidates = std::move(*available);
    }

    if (const auto matched = find_exact_model_reference_match(term, candidates)) {
        const auto model = *matched;
        // Re-resolve the session after the availability suspension: a
        // session replacement may have swapped the active session while the
        // refresh was in flight (the pre-extraction member re-read shape).
        auto* target = current_session(hooks_);
        if (target == nullptr) co_return;
        auto switched = co_await target->set_model(model);
        if (!switched) {
            presenter_->show_error(combined_error_text(switched.error()));
            co_return;
        }
        update_model_completion();
        presenter_->show_status("Model: " + model.id);
        co_return;
    }
    show_model_selector(term);
}

/// The selector's select flow (pi `handleSelect`): `session.setModel` on the
/// executor, then the `Model: <id>` status from the session that is current
/// when the switch settles.
boost::asio::awaitable<void> ModelFlowController::run_model_switch(cch::ai::Model model) {
    auto* session = current_session(hooks_);
    if (session == nullptr) co_return;
    auto switched = co_await session->set_model(std::move(model));
    if (!switched) {
        presenter_->show_error(combined_error_text(switched.error()));
        co_return;
    }
    update_model_completion();
    presenter_->restore_prompt_slot();
    if (auto* current = current_session(hooks_); current != nullptr) {
        presenter_->show_status("Model: " + current->snapshot().agent_state.model.id);
    }
}

// ── Model cycle (pi `cycleModel`) ────────────────────────────────────────

/// pi `cycleModel` presentation: `Only one model in scope` / `Only one
/// model available` when the cycle cannot move, otherwise the `Switched to
/// <name> (thinking: <level>)` status; errors surface as diagnostic lines.
boost::asio::awaitable<void> ModelFlowController::run_model_cycle(std::string direction) {
    auto* session = current_session(hooks_);
    if (session == nullptr) co_return;
    auto result = co_await session->cycle_model(std::move(direction));
    if (!result) {
        presenter_->show_error(combined_error_text(result.error()));
        co_return;
    }
    if (!*result) {
        const auto* current = current_session(hooks_);
        presenter_->show_status(
            current == nullptr || current->scoped_models().empty() ? "Only one model available"
                                                                   : "Only one model in scope");
        co_return;
    }
    const auto& cycle = **result;
    update_model_completion();
    const auto thinking_str =
        cycle.model.reasoning && cycle.thinking_level != "off"
        ? " (thinking: " + cycle.thinking_level + ")"
        : "";
    const auto label = cycle.model.name.empty() ? cycle.model.id : cycle.model.name;
    presenter_->show_status("Switched to " + label + thinking_str);
}

// ── Scoped models selector (pi `showModelsSelector`) ─────────────────────

/// pi `showModelsSelector` / `updateSessionModels`/`onPersist`: the
/// scoped-models selector starts from the session scope, else the settings
/// `enabledModels` patterns resolved over the live availability (no-match
/// diagnostics become unavailable ids); changes stay session-only until
/// `app.models.save` persists them.
boost::asio::awaitable<void> ModelFlowController::run_scoped_models_selector() {
    if (hooks_.is_live == nullptr || !hooks_.is_live()) co_return;
    if (hooks_.live_theme == nullptr) co_return;
    auto* session = current_session(hooks_);
    if (session == nullptr || !session->is_open()) co_return;
    auto runtime = session->model_runtime();
    if (!runtime) co_return;
    // pi: refresh() then getAvailable().
    (void)runtime->refresh();
    auto available = co_await ai::detail::await_async_result(runtime->get_available());
    if (!available) {
        presenter_->show_error(combined_error_text(available.error()));
        co_return;
    }
    // Re-resolve the session after the availability suspension: a session
    // replacement may have swapped the active session while the refresh was
    // in flight (the pre-extraction member re-read shape).
    session = current_session(hooks_);
    if (session == nullptr) co_return;
    const auto all_models = std::move(*available);
    std::set<std::string, std::less<>> all_model_ids;
    for (const auto& model : all_models) {
        all_model_ids.insert(model.provider + "/" + model.id);
    }
    const std::vector<std::string>* configured_patterns =
        settings_manager_ && settings_manager_->settings().enabled_models
        ? &*settings_manager_->settings().enabled_models
        : nullptr;
    const auto& session_scoped_models = session->scoped_models();
    if (all_models.empty() &&
        (configured_patterns == nullptr || configured_patterns->empty()) &&
        session_scoped_models.empty()) {
        presenter_->show_status("No models available");
        co_return;
    }

    std::optional<ModelScopeResolution> configured_scope;
    if (configured_patterns != nullptr && !configured_patterns->empty()) {
        configured_scope =
            resolve_model_scope_with_diagnostics(*configured_patterns, all_models);
    }
    auto current_enabled_ids = initial_selector_enabled_ids(
        session_scoped_models, configured_scope);

    const auto weak = weak_from_this();
    const auto all_models_shared = std::make_shared<const std::vector<cch::ai::Model>>(all_models);
    const auto all_model_ids_shared = std::make_shared<const std::set<std::string, std::less<>>>(all_model_ids);
    auto selector = std::make_shared<ScopedModelsSelectorComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        all_models,
        std::move(current_enabled_ids),
        [weak, all_models = all_models_shared, ids = all_model_ids_shared](
            std::optional<std::vector<std::string>> enabled_ids) {
            // Input-thread sink: apply session-only scope changes on the
            // executor (pi `updateSessionModels`).
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor(
                    [self,
                     enabled_ids = std::move(enabled_ids),
                     all_models = std::move(all_models),
                     ids = std::move(ids)]() mutable {
                        self->apply_scoped_model_change(
                            std::move(enabled_ids), *all_models, *ids);
                    });
            }
        },
        [weak, all_models = all_models_shared, ids = all_model_ids_shared](
            std::optional<std::vector<std::string>> enabled_ids) {
            // Input-thread sink: persist to settings on the executor (pi
            // `onPersist`).
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor(
                    [self,
                     enabled_ids = std::move(enabled_ids),
                     all_models = std::move(all_models),
                     ids = std::move(ids)]() mutable {
                        self->persist_scoped_models(
                            std::move(enabled_ids), *all_models, *ids);
                    });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) {
                self->hooks_.post_on_executor([self] { self->presenter_->restore_prompt_slot(); });
            }
        });
    presenter_->replace_prompt_slot(std::move(selector));
}

/// pi `updateSessionModels` (executor): session-only scope changes from the
/// scoped-models selector. An explicit list with at least one available
/// model and not every available model enabled resolves to the session
/// scope; otherwise the scope clears (all enabled / none enabled = no
/// filter).
void ModelFlowController::apply_scoped_model_change(
    std::optional<std::vector<std::string>> enabled_ids,
    const std::vector<cch::ai::Model>& all_models,
    const std::set<std::string, std::less<>>& all_model_ids) {
    const bool has_enabled_available =
        enabled_ids && std::any_of(
                           enabled_ids->begin(),
                           enabled_ids->end(),
                           [&](const std::string& id) { return all_model_ids.contains(id); });
    const bool all_available_enabled =
        enabled_ids && std::all_of(
                           all_model_ids.begin(),
                           all_model_ids.end(),
                           [&](const std::string& id) {
                               return std::find(
                                          enabled_ids->begin(),
                                          enabled_ids->end(),
                                          id) != enabled_ids->end();
                           });
    auto* session = current_session(hooks_);
    if (session == nullptr) return;
    if (enabled_ids && has_enabled_available && !all_available_enabled) {
        session->set_scoped_models(
            resolve_model_scope(*enabled_ids, all_models));
    } else {
        session->set_scoped_models({});
    }
    update_model_completion();
    presenter_->invalidate();
}

/// pi `onPersist` (executor): persist the current selection to the global
/// `enabledModels` settings field; an all-enabled selection clears the field
/// (pi writes `undefined`).
void ModelFlowController::persist_scoped_models(
    std::optional<std::vector<std::string>> enabled_ids,
    const std::vector<cch::ai::Model>& all_models,
    const std::set<std::string, std::less<>>& all_model_ids) {
    const bool all_enabled =
        enabled_ids && enabled_ids->size() == all_models.size() &&
        std::all_of(
            enabled_ids->begin(),
            enabled_ids->end(),
            [&](const std::string& id) { return all_model_ids.contains(id); });
    const auto new_patterns =
        !enabled_ids || all_enabled ? std::nullopt : std::move(enabled_ids);
    if (settings_manager_) {
        (void)settings_manager_->set_enabled_models(std::move(new_patterns));
    }
    presenter_->show_status("Model selection saved to settings");
}

// ── Flow spawning ────────────────────────────────────────────────────────

void ModelFlowController::spawn(
    std::move_only_function<boost::asio::awaitable<void>()> start,
    std::string failure_label) {
    if (hooks_.spawn_flow == nullptr) return;
    auto self = shared_from_this();
    auto host_lifetime = host_lifetime_.lock();
    if (host_lifetime == nullptr) return;
    // The coroutine frame references its closure (the `start` factory) and
    // the presenter/session/settings referents, so the closure, the
    // controller, and the host all stay alive until the spawned coroutine
    // reaches its terminal completion — the pre-extraction shared-state
    // capture shape.
    hooks_.spawn_flow(
        [self, host_lifetime = std::move(host_lifetime), start = std::move(start)]() mutable
            -> boost::asio::awaitable<void> {
            co_await start();
        },
        std::move(failure_label));
}

} // namespace cch::coding_agent::tui
