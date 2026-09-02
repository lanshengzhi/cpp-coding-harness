// Headless ModelFlowController coverage (#503): the `/model` selector flow,
// the `/models` scoped-models flow, and Ctrl+P cycling run against a
// recording ModalPresenter and a real AgentSession over a temp Agent Config
// Directory — no virtual terminal, no live credentials, no network.

#include "coding_agent/tui/ModelFlowController.hpp"

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/ModelSelector.hpp"
#include "coding_agent/tui/ScopedModelsSelector.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/PumpUntil.hpp"
#include "support/RuntimeFixture.hpp"
#include "support/RuntimeLoopDriver.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/Keys.hpp>
#include <cch/tui/Overlay.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;
using tests::drain_ready;

namespace {

/// Records every ModalPresenter call; the prompt slot holds the last
/// replacement component for type inspection.
class RecordingModalPresenter final : public coding_agent::tui::ModalPresenter {
public:
    void show_overlay(std::unique_ptr<cch::tui::Overlay>) override {
        ++overlays_shown;
    }
    void close_overlay() override { ++overlays_closed; }
    void replace_prompt_slot(std::shared_ptr<cch::tui::Component> component) override {
        slot = std::move(component);
        ++slot_replacements;
    }
    void restore_prompt_slot() override {
        slot.reset();
        ++slot_restores;
    }
    void show_status(std::string text) override { statuses.push_back(std::move(text)); }
    void show_error(std::string text) override { errors.push_back(std::move(text)); }
    void request_render() override { ++render_requests; }
    void invalidate() override { ++invalidations; }

    std::shared_ptr<cch::tui::Component> slot;
    int overlays_shown{0};
    int overlays_closed{0};
    int slot_replacements{0};
    int slot_restores{0};
    int render_requests{0};
    int invalidations{0};
    std::vector<std::string> statuses;
    std::vector<std::string> errors;
};

/// `alpha` (keyed, reasoning) and `beta` (keyed, non-reasoning).
constexpr std::string_view kReasoningAndPlainKeyed = R"({
  "providers": {
    "alpha": {
      "baseUrl": "https://alpha.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-alpha-key",
      "models": [{"id": "alpha-1", "name": "Alpha Reasoning", "reasoning": true}]
    },
    "beta": {
      "baseUrl": "https://beta.example/v1",
      "api": "openai-responses",
      "apiKey": "dummy-beta-key",
      "models": [{"id": "beta-1", "name": "Beta Plain", "reasoning": false}]
    }
  }
})";

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    // The scoped-models selector's six `app.models.*` actions are
    // application-level definitions (the host registers them through the
    // keybindings manager); mirror the production binding set.
    const auto app_action = [&request](std::string_view id, std::vector<std::string> keys) {
        request.definitions.push_back({
            .id = std::string{id},
            .default_keys = std::move(keys),
            .description = {},
            .category = {},
        });
    };
    app_action("app.models.save", {"ctrl+s"});
    app_action("app.models.enableAll", {"ctrl+a"});
    app_action("app.models.clearAll", {"ctrl+x"});
    app_action("app.models.toggleProvider", {"ctrl+p"});
    app_action("app.models.reorderUp", {"alt+up"});
    app_action("app.models.reorderDown", {"alt+down"});
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

/// One isolated assembly: a temp workspace for the session file and a temp
/// Agent Config Directory (`PI_CODING_AGENT_DIR`) whose models.json and
/// settings.json drive runtime creation deterministically, plus the host
/// hooks wired to the fixture's io_context like the interactive host's
/// executor.
struct ModelFlowFixture {
    cch::tests::TempWorkspace workspace;
    cch::tests::TempWorkspace agent_dir;
    tests::EnvVarGuard dir_guard{"PI_CODING_AGENT_DIR"};
    tests::EnvVarGuard home_guard{"HOME"};
    tests::EnvVarGuard kimi_guard{"KIMI_API_KEY"};

    boost::asio::io_context io;
    RecordingModalPresenter presenter;
    coding_agent::tui::LiveTheme theme{
        coding_agent::tui::builtin_dark_theme(),
        cch::tui::TerminalColorCapability::TrueColor};
    std::shared_ptr<void> host_token{std::make_shared<int>(0)};

    tests::RuntimeFixture runtime;
    std::unique_ptr<coding_agent::AgentSession> session;
    std::optional<tests::RuntimeLoopDriver> runtime_driver{std::nullopt};
    std::optional<coding_agent::SettingsManager> settings{std::nullopt};
    std::shared_ptr<coding_agent::tui::ModelFlowController> flows;

    ModelFlowFixture() {
        dir_guard.set(agent_dir.path().string());
        home_guard.set(workspace.path().string());
        kimi_guard.unset();
    }

    void write_models(std::string_view json) {
        std::ofstream out(agent_dir.path() / "models.json", std::ios::binary);
        out << json;
    }

    [[nodiscard]] std::string read_settings() const {
        std::ifstream in(agent_dir.path() / "settings.json", std::ios::binary);
        return std::string{
            std::istreambuf_iterator<char>{in},
            std::istreambuf_iterator<char>{}};
    }

    void boot(std::vector<std::string> models = {}) {
        coding_agent::runtime::AgentSessionCreationRequest request;
        request.session_facts.no_skills = true;
        request.session_facts.no_prompt_templates = true;
        request.workspace = workspace.path();
        request.session_target =
            coding_agent::ExplicitOpenOrCreateSessionTarget{workspace.path() / "session.jsonl"};
        request.session_facts.models = std::move(models);
        request.execution_runtime_target = runtime.make_target();
        auto created = runtime.run(coding_agent::create_agent_session_async(std::move(request), std::nullopt, {}));
        REQUIRE(created.has_value());
        session = std::move(created->session);
        runtime_driver.emplace(runtime);
        settings.emplace(
            coding_agent::SettingsManager::create({}, agent_dir.path(), false));

        coding_agent::tui::ModelFlowHostHooks hooks;
        hooks.is_live = [] { return true; };
        hooks.post_on_executor =
            [this](std::move_only_function<void()> action) mutable {
                boost::asio::post(io, std::move(action));
            };
        hooks.spawn_flow =
            [this](std::move_only_function<boost::asio::awaitable<void>()> start,
                   std::string failure_label) mutable {
                (void)failure_label;
                auto owner = std::make_shared<
                    std::move_only_function<boost::asio::awaitable<void>()>>(std::move(start));
                boost::asio::co_spawn(
                    io,
                    [owner]() mutable -> boost::asio::awaitable<void> { co_await (*owner)(); },
                    boost::asio::detached);
            };
        auto* session_pointer = session.get();
        hooks.current_session = [session_pointer]() -> coding_agent::AgentSession* {
            return session_pointer;
        };
        hooks.live_theme = [this]() -> const coding_agent::tui::LiveTheme& { return theme; };
        flows = std::make_shared<coding_agent::tui::ModelFlowController>(
            io.get_executor(),
            presenter,
            std::weak_ptr<void>{host_token},
            std::move(hooks),
            std::make_shared<coding_agent::tui::SharedKeybindings>(test_keybindings()),
            &*settings);
        flows->update_model_completion();
    }

    void drain() { drain_ready(io); }
};

} // namespace

TEST_CASE(
    "ModelFlowController cycles forward and backward with pi statuses through the presenter",
    "[coding_agent][tui][model-flows][issue503]") {
    ModelFlowFixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    fixture.boot();
    CHECK(fixture.session->model() == "alpha-1");

    fixture.flows->cycle_model("forward");
    fixture.drain();
    CHECK(fixture.session->snapshot().agent_state.model.id == "beta-1");
    REQUIRE_FALSE(fixture.presenter.statuses.empty());
    CHECK(fixture.presenter.statuses.back() == "Switched to Beta Plain");

    fixture.flows->cycle_model("backward");
    fixture.drain();
    CHECK(fixture.session->snapshot().agent_state.model.id == "alpha-1");
    CHECK(fixture.presenter.statuses.back() == "Switched to Alpha Reasoning (thinking: medium)");
    CHECK(fixture.presenter.errors.empty());
    // The cycle never touches the prompt slot.
    CHECK(fixture.presenter.slot_replacements == 0);
}

TEST_CASE(
    "ModelFlowController reports the one-model scope status when the cycle cannot move",
    "[coding_agent][tui][model-flows][issue503]") {
    ModelFlowFixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    fixture.boot({"alpha/alpha-1"});
    CHECK(fixture.session->scoped_models().size() == 1);

    fixture.flows->cycle_model("forward");
    fixture.drain();
    CHECK(fixture.session->snapshot().agent_state.model.id == "alpha-1");
    REQUIRE_FALSE(fixture.presenter.statuses.empty());
    CHECK(fixture.presenter.statuses.back() == "Only one model in scope");
}

TEST_CASE(
    "ModelFlowController switches on an exact /model reference and opens the selector otherwise",
    "[coding_agent][tui][model-flows][issue503]") {
    ModelFlowFixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    fixture.boot();

    // An exact provider/model reference switches immediately (pi
    // `handleModelCommand`) without opening the selector.
    fixture.flows->open_model_selector("beta/beta-1");
    fixture.drain();
    CHECK(fixture.session->snapshot().agent_state.model.id == "beta-1");
    REQUIRE_FALSE(fixture.presenter.statuses.empty());
    CHECK(fixture.presenter.statuses.back() == "Model: beta-1");
    CHECK(fixture.presenter.slot == nullptr);
    CHECK(fixture.presenter.errors.empty());

    // A term with no exact match opens the selector in the prompt slot.
    fixture.flows->open_model_selector("zzz");
    fixture.drain();
    auto selector =
        std::dynamic_pointer_cast<coding_agent::tui::ModelSelectorComponent>(
            fixture.presenter.slot);
    CHECK(selector != nullptr);
    CHECK(fixture.session->snapshot().agent_state.model.id == "beta-1");

    // An empty term opens the selector too (pi `handleModelCommand`).
    fixture.flows->open_model_selector("");
    fixture.drain();
    selector = std::dynamic_pointer_cast<coding_agent::tui::ModelSelectorComponent>(
        fixture.presenter.slot);
    CHECK(selector != nullptr);
    CHECK(fixture.presenter.errors.empty());
}

TEST_CASE(
    "ModelFlowController opens the scoped-models selector through the presenter",
    "[coding_agent][tui][model-flows][issue503]") {
    ModelFlowFixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    fixture.boot();

    fixture.flows->open_scoped_models_selector();
    fixture.drain();
    auto selector =
        std::dynamic_pointer_cast<coding_agent::tui::ScopedModelsSelectorComponent>(
            fixture.presenter.slot);
    CHECK(selector != nullptr);
    CHECK(fixture.presenter.errors.empty());
    // Opening the selector never mutates the session scope.
    CHECK(fixture.session->scoped_models().empty());
}

TEST_CASE(
    "ModelFlowController applies scoped-models selector changes to the session scope only",
    "[coding_agent][tui][model-flows][issue503]") {
    ModelFlowFixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    fixture.boot();

    fixture.flows->open_scoped_models_selector();
    fixture.drain();
    auto selector =
        std::dynamic_pointer_cast<coding_agent::tui::ScopedModelsSelectorComponent>(
            fixture.presenter.slot);
    REQUIRE(selector != nullptr);

    // pi `toggle`: the first toggle on the all-enabled state starts an
    // explicit list with only the highlighted id enabled. The change sink
    // hops to the executor and narrows the session scope (pi
    // `updateSessionModels`) without touching settings.
    selector->handle_input(tui::KeyEvent{.key = "enter"});
    fixture.drain();
    CHECK(fixture.session->scoped_models().size() == 1);
    CHECK(fixture.session->scoped_models().front().model.id == "alpha-1");
    // The `/model` completion snapshot follows the narrowed scope.
    REQUIRE(fixture.flows->model_completion() != nullptr);
    CHECK(fixture.flows->model_completion()->size() == 1);
    CHECK(fixture.presenter.invalidations > 0);
    // Session-only: nothing persisted to settings yet.
    CHECK(fixture.read_settings().find("enabledModels") == std::string::npos);

    // Toggling the only enabled model off empties the explicit list, which
    // resolves to no session scope (pi: none enabled = no filter).
    selector->handle_input(tui::KeyEvent{.key = "enter"});
    fixture.drain();
    CHECK(fixture.session->scoped_models().empty());
    CHECK(fixture.flows->model_completion()->size() == 2);
    CHECK(fixture.presenter.errors.empty());
}

TEST_CASE(
    "ModelFlowController persists the scoped-models selection on save and clears it when all enabled",
    "[coding_agent][tui][model-flows][issue503]") {
    ModelFlowFixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    fixture.boot();

    fixture.flows->open_scoped_models_selector();
    fixture.drain();
    auto selector =
        std::dynamic_pointer_cast<coding_agent::tui::ScopedModelsSelectorComponent>(
            fixture.presenter.slot);
    REQUIRE(selector != nullptr);

    // Narrow to alpha-1, then save (pi `app.models.save` → `onPersist`):
    // the explicit list lands in the global `enabledModels` field.
    selector->handle_input(tui::KeyEvent{.key = "enter"});
    fixture.drain();
    selector->handle_input(tui::KeyEvent{.key = "s", .ctrl = true});
    fixture.drain();
    CHECK(fixture.read_settings().find("alpha/alpha-1") != std::string::npos);
    REQUIRE_FALSE(fixture.presenter.statuses.empty());
    CHECK(fixture.presenter.statuses.back() == "Model selection saved to settings");

    // Enable-all normalizes back to the null (all-enabled) state, which the
    // change sink applies as no session scope; saving then removes the
    // settings field (pi writes `undefined`).
    selector->handle_input(tui::KeyEvent{.key = "a", .ctrl = true});
    fixture.drain();
    CHECK(fixture.session->scoped_models().empty());
    selector->handle_input(tui::KeyEvent{.key = "s", .ctrl = true});
    fixture.drain();
    CHECK(fixture.read_settings().find("enabledModels") == std::string::npos);
    CHECK(fixture.presenter.statuses.back() == "Model selection saved to settings");
    CHECK(fixture.presenter.errors.empty());
}

TEST_CASE(
    "ModelFlowController builds the /model completion snapshot from the scope or availability",
    "[coding_agent][tui][model-flows][issue503]") {
    ModelFlowFixture fixture;
    fixture.write_models(kReasoningAndPlainKeyed);
    fixture.boot();

    // No session scope: the availability snapshot (pi `getModelCandidates`).
    auto completion = fixture.flows->model_completion();
    REQUIRE(completion != nullptr);
    REQUIRE(completion->size() == 2);
    CHECK((*completion)[0].provider == "alpha");
    CHECK((*completion)[0].id == "alpha-1");
    CHECK((*completion)[1].id == "beta-1");

    // A session scope narrows the candidates (pi's scoped candidate set).
    const auto available = fixture.session->model_runtime()->get_available_snapshot();
    fixture.session->set_scoped_models(
        coding_agent::resolve_model_scope({"alpha/alpha-1"}, available));
    fixture.flows->update_model_completion();
    completion = fixture.flows->model_completion();
    REQUIRE(completion->size() == 1);
    CHECK(completion->front().id == "alpha-1");
}
