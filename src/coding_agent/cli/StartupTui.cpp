#include "coding_agent/cli/StartupTui.hpp"

#include "coding_agent/SessionCwd.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/SessionSelector.hpp"
#include "coding_agent/tui/SelectListPrompt.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/ThemeController.hpp"
#include "coding_agent/tui/TerminationSignals.hpp"
#include "support/ExpectedMacros.hpp"

#include <cch/support/Error.hpp>
#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Tui.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace cch::cli {
namespace {

struct StartupDelayedTuiWork {
    explicit StartupDelayedTuiWork(boost::asio::any_io_executor executor) : timer(std::move(executor)) {}
    boost::asio::steady_timer timer;
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
};

class StartupTuiScheduledHandle final : public cch::tui::TuiScheduledHandle {
public:
    explicit StartupTuiScheduledHandle(std::shared_ptr<StartupDelayedTuiWork> state) : state_(std::move(state)) {}
    ~StartupTuiScheduledHandle() override { cancel(); }
    void cancel() noexcept override {
        if (!state_) return;
        *state_->alive = false;
        (void)state_->timer.cancel();
        state_.reset();
    }

private:
    std::shared_ptr<StartupDelayedTuiWork> state_;
};

class StartupTuiScheduler final : public cch::tui::TuiScheduler {
public:
    explicit StartupTuiScheduler(boost::asio::any_io_executor executor)
        : executor_(std::move(executor)), timer_(executor_), interval_timer_(executor_) {}

    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return std::chrono::steady_clock::now();
    }

    void post(cch::tui::TuiScheduledWork work) override {
        boost::asio::post(executor_, [work = std::move(work)]() mutable {
            if (work) work();
        });
    }

    void schedule(std::chrono::steady_clock::duration delay, cch::tui::TuiScheduledWork work) override {
        timer_.expires_after(std::max(delay, std::chrono::steady_clock::duration::zero()));
        timer_.async_wait([work = std::move(work)](const boost::system::error_code& error) mutable {
            if (!error && work) work();
        });
    }

    [[nodiscard]] std::unique_ptr<cch::tui::TuiScheduledHandle> schedule_delayed(
            std::chrono::steady_clock::duration delay, cch::tui::TuiScheduledWork work) override {
        delayed_work_.erase(
                std::remove_if(
                        delayed_work_.begin(), delayed_work_.end(), [](const auto& item) { return item.expired(); }),
                delayed_work_.end());
        auto state = std::make_shared<StartupDelayedTuiWork>(executor_);
        delayed_work_.push_back(state);
        state->timer.expires_after(std::max(delay, std::chrono::steady_clock::duration::zero()));
        state->timer.async_wait([state, work = std::move(work)](const boost::system::error_code& error) mutable {
            if (error || !*state->alive) return;
            *state->alive = false;
            if (work) work();
        });
        return std::make_unique<StartupTuiScheduledHandle>(std::move(state));
    }

    void cancel() noexcept override { (void)timer_.cancel(); }

    void schedule_interval(std::chrono::steady_clock::duration period, cch::tui::TuiScheduledWork work) override {
        (void)interval_timer_.cancel();
        interval_period_ = std::max(period, std::chrono::steady_clock::duration::zero());
        interval_work_ = std::make_shared<cch::tui::TuiScheduledWork>(std::move(work));
        arm_interval();
    }

    void cancel_interval() noexcept override {
        interval_work_.reset();
        (void)interval_timer_.cancel();
    }

    /// CODING_STANDARDS §6.2: async callbacks must not touch a dying owner.
    /// This token follows AltScreen's RenderScheduleState weak-owner/generation pattern.
    ~StartupTuiScheduler() override {
        for (const auto& weak_state : delayed_work_) {
            if (const auto state = weak_state.lock()) {
                *state->alive = false;
                (void)state->timer.cancel();
            }
        }
        *interval_alive_ = false;
        interval_work_.reset();
    }

private:
    void arm_interval() {
        interval_timer_.expires_after(interval_period_);
        interval_timer_.async_wait([alive = interval_alive_, this](const boost::system::error_code& error) {
            if (!*alive) return;
            const auto work = interval_work_;
            if (error || !work || !*work) return;
            arm_interval();
            (*work)();
        });
    }

    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer timer_;
    boost::asio::steady_timer interval_timer_;
    std::chrono::steady_clock::duration interval_period_{};
    std::shared_ptr<cch::tui::TuiScheduledWork> interval_work_;
    std::shared_ptr<std::atomic_bool> interval_alive_ = std::make_shared<std::atomic_bool>(true);
    std::vector<std::weak_ptr<StartupDelayedTuiWork>> delayed_work_;
};

/// One awaiting startup-TUI outcome (pi `startup-ui.ts` promise): first
/// resolution wins, every callback hops to the consumer executor so the Tui
/// is only ever touched from one thread. Render requests are dropped once
/// the coroutine has finished tearing the host down.
template <typename T> struct StartupSlot : std::enable_shared_from_this<StartupSlot<T>> {
    explicit StartupSlot(boost::asio::any_io_executor executor)
        : executor(std::move(executor)), channel(this->executor, 1) {}

    void resolve(support::Expected<T> value) {
        if (resolved.exchange(true)) return;
        const auto self = this->shared_from_this();
        boost::asio::post(executor, [self, value = std::move(value)]() mutable {
            self->channel.try_send(boost::system::error_code{}, std::move(value));
        });
    }

    void request_render(cch::tui::ITui* tui) {
        const auto self = this->shared_from_this();
        boost::asio::post(executor, [self, tui] {
            if (!self->finished) tui->request_render();
        });
    }

    boost::asio::any_io_executor executor;
    boost::asio::experimental::concurrent_channel<void(boost::system::error_code, support::Expected<T>)> channel;
    std::atomic<bool> resolved{false};
    bool finished{false};
};

/// pi `createStartupTui` `setKeybindings(KeybindingsManager.create())`: the
/// startup registry reads <Agent Config Directory>/keybindings.json over the
/// assembled actions (the session-selector actions for the picker, the tui
/// builtins alone for the generic selector prompt).
[[nodiscard]] support::Expected<std::shared_ptr<const cch::tui::KeybindingRegistry>> startup_keybindings(
        const StartupTuiOptions& options, std::span<const std::string_view> application_actions) {
    auto definitions = coding_agent::tui::app_keybinding_definitions(application_actions);
    if (!definitions) {
        return std::unexpected(definitions.error());
    }
    auto manager = coding_agent::tui::load_keybindings_manager({
            .agent_config_directory = options.agent_config_directory,
            .application_definitions = std::move(*definitions),
    });
    if (!manager) {
        return std::unexpected(manager.error());
    }
    return std::move(manager->registry);
}

/// pi `createStartupTui` theme init (G5 controller default): the
/// settings-resolved theme or the COLORFGBG env default through
/// `init_boot_theme`, with pi's silent dark fallback. Theme registration is
/// skipped (pi `setRegisteredThemes(loadStartupThemes(...))` stays absent).
[[nodiscard]] cch::coding_agent::tui::LiveTheme start_live_theme(
        const StartupTuiOptions& options, const cch::tui::Terminal& terminal) {
    auto resolved = coding_agent::tui::init_boot_theme(options.agent_config_directory, options.theme_setting);
    return cch::coding_agent::tui::LiveTheme{std::move(resolved), terminal.capabilities().color};
}

/// Run one startup-TUI coroutine on a fresh ProcessTerminal + io_context and
/// return its outcome (pi main.ts running `selectSession` /
/// `promptForMissingSessionCwd` before the main TUI on its own terminal).
template <typename T, typename Coroutine>
[[nodiscard]] support::Expected<T> run_process_terminal_host(Coroutine coroutine) {
    boost::asio::io_context io;
    const cch::tui::ProcessTerminalOptions terminal_options{.executor = io.get_executor()};
    // The startup hosts own the terminal before the main TUI exists (pi
    // `selectSession` / `promptForMissingSessionCwd`), so they claim the
    // termination signals for their own raw-mode window too. A failed claim only
    // means this boot host runs without the guarantee.
    coding_agent::tui::TerminationSignals termination;
    (void)termination.arm_terminal(terminal_options.input_fd, terminal_options.output_fd);
    cch::tui::ProcessTerminal terminal(terminal_options);
    auto future = boost::asio::co_spawn(io, std::move(coroutine)(terminal), boost::asio::use_future);
    io.run();
    auto result = future.get();
    if (!result) {
        return std::unexpected(result.error());
    }
    return std::move(*result);
}

} // namespace

/// pi `startup-ui.ts` host lifecycle shared by the picker and the
/// missing-cwd prompt: the startup registry (keybindings.json read), the
/// G5 controller-default theme init, the Tui boot, one selector component,
/// and the clear-and-stop teardown (pi `createStartupTui` +
/// `startStartupTui` + `clearStartupTui`). `build_selector` wires the
/// component's sinks to the slot and the host's render-request hook; first
/// resolution wins.
template <typename T, typename BuildSelector>
[[nodiscard]] boost::asio::awaitable<support::Expected<T>> run_startup_host(cch::tui::Terminal& terminal,
        StartupTuiOptions options,
        std::span<const std::string_view> keybinding_actions,
        BuildSelector build_selector) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto slot = std::make_shared<StartupSlot<T>>(executor);

    // pi `createStartupTui`: the startup registry reads keybindings.json
    // over the assembled actions (pi `KeybindingsManager.create`).
    auto keybindings = startup_keybindings(options, keybinding_actions);
    if (!keybindings) {
        co_return std::unexpected(keybindings.error());
    }

    StartupTuiScheduler scheduler{executor};
    cch::tui::Tui tui{terminal};
    tui.set_scheduler(&scheduler);
    CCH_TRY_VOID(tui.start());
    // The detected capabilities (color depth, ...) are known only after the
    // terminal starts (pi `createStartupTui` constructs the palette after
    // the TUI is up; the controller default init renders at that depth).
    cch::coding_agent::tui::LiveTheme live_theme = start_live_theme(options, terminal);
    const auto request_render = [slot, &tui]() -> support::ExpectedVoid {
        slot->request_render(&tui);
        return {};
    };

    auto selector = build_selector(live_theme, *keybindings, slot, std::move_only_function<void()>{[slot, &tui] {
        slot->request_render(&tui);
    }});
    auto* selector_ptr = selector.get();
    CCH_TRY_VOID(tui.add_child(std::move(selector)));
    CCH_TRY_VOID(tui.set_focus(selector_ptr));
    tui.request_render(true);

    auto result = co_await slot->channel.async_receive(boost::asio::use_awaitable);

    // pi `clearStartupTui` + `ui.stop()`: clear the physical screen and
    // restore the terminal before the next host takes it over.
    slot->finished = true;
    (void)tui.clear_screen();
    (void)tui.stop();
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return std::move(*result);
}

boost::asio::awaitable<support::Expected<StartupPickerResult>> run_startup_session_picker(cch::tui::Terminal& terminal,
        StartupTuiOptions options,
        coding_agent::tui::SessionListLoader current_loader,
        coding_agent::tui::SessionListLoader all_loader) {
    // pi `createStartupTui`: the startup registry reads keybindings.json
    // over the session-selector actions (pi `KeybindingsManager.create`).
    static constexpr std::string_view kSessionActions[] = {
            "app.session.toggleSort",
            "app.session.toggleNamedFilter",
            "app.session.togglePath",
            "app.session.rename",
            "app.session.delete",
            "app.session.deleteNoninvasive",
    };

    // pi `selectSession`: the startup session selector with
    // `showRenameHint: false` (no rename sink → the rename hint stays
    // hidden) and no current-session marker. Cancel and the exit binding
    // resolve without a path (pi selectSession cancel → null; the baseline
    // component never fires onExit from handleInput, like pi's SessionList).
    return run_startup_host<StartupPickerResult>(terminal,
            std::move(options),
            kSessionActions,
            [current_loader = std::move(current_loader), all_loader = std::move(all_loader)](
                    const cch::coding_agent::tui::LiveTheme& live_theme,
                    const std::shared_ptr<const cch::tui::KeybindingRegistry>& keybindings,
                    const std::shared_ptr<StartupSlot<StartupPickerResult>>& slot,
                    std::move_only_function<void()> request_render) mutable -> std::unique_ptr<cch::tui::Component> {
                return std::make_unique<coding_agent::tui::SessionSelectorComponent>(
                        live_theme,
                        keybindings,
                        std::move(current_loader),
                        std::move(all_loader),
                        std::nullopt,
                        [slot](std::string session_path) {
                            slot->resolve(StartupPickerResult{
                                    .outcome = StartupPickerOutcome::Selected,
                                    .session_path = std::move(session_path),
                            });
                        },
                        [slot] {
                            slot->resolve(StartupPickerResult{
                                    .outcome = StartupPickerOutcome::Cancelled,
                                    .session_path = std::nullopt,
                            });
                        },
                        [slot] {
                            slot->resolve(StartupPickerResult{
                                    .outcome = StartupPickerOutcome::Exited,
                                    .session_path = std::nullopt,
                            });
                        },
                        nullptr,
                        // pi selectSession passes onRequestRender → ui.requestRender:
                        // every selector mutation repaints through the host. The
                        // hook is a host-frame local; the component lives inside the
                        // host frame.
                        [request_render = std::move(request_render)]() mutable { (void)request_render(); });
            });
}

boost::asio::awaitable<support::Expected<bool>> run_startup_missing_cwd_prompt(
        cch::tui::Terminal& terminal, StartupTuiOptions options, std::string title) {
    // pi `createStartupTui` with the generic selector: the registry reads
    // keybindings.json over the tui builtins alone.
    return run_startup_host<bool>(terminal,
            std::move(options),
            {},
            [title = std::move(title)](const cch::coding_agent::tui::LiveTheme& live_theme,
                    const std::shared_ptr<const cch::tui::KeybindingRegistry>& keybindings,
                    const std::shared_ptr<StartupSlot<bool>>& slot,
                    std::move_only_function<void()>) mutable -> std::unique_ptr<cch::tui::Component> {
                // pi `showStartupSelector`: Continue → the fallback cwd, Cancel
                // → undefined (the boot exits 0). Each option is a SelectItem
                // with value == label.
                auto list = std::make_unique<cch::tui::SelectList>(
                        std::vector<cch::tui::SelectItem>{
                                {.value = "Continue", .label = "Continue"},
                                {.value = "Cancel", .label = "Cancel"},
                        },
                        cch::tui::SelectListOptions{
                                .theme = live_theme.select_list_theme(),
                                .on_select = [slot](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                                    slot->resolve(item.value == "Continue");
                                    return {};
                                },
                                .on_cancel = [slot]() -> support::ExpectedVoid {
                                    slot->resolve(false);
                                    return {};
                                },
                                .keybindings = keybindings,
                        });
                return std::make_unique<coding_agent::tui::SelectListPrompt>(std::move(title),
                        coding_agent::tui::generic_select_list_hint(*keybindings),
                        live_theme.foreground_hook(cch::coding_agent::tui::ThemeToken::Border),
                        std::move(list));
            });
}

support::Expected<std::optional<std::filesystem::path>> run_process_terminal_resume_picker(StartupTuiOptions options,
        coding_agent::tui::SessionListLoader current_loader,
        coding_agent::tui::SessionListLoader all_loader) {
    auto result = run_process_terminal_host<StartupPickerResult>(
            [options = std::move(options),
                    current_loader = std::move(current_loader),
                    all_loader = std::move(all_loader)](cch::tui::Terminal& terminal) mutable {
                return run_startup_session_picker(
                        terminal, std::move(options), std::move(current_loader), std::move(all_loader));
            });
    if (!result) {
        return std::unexpected(result.error());
    }
    switch (result->outcome) {
    case StartupPickerOutcome::Selected:
        return result->session_path;
    case StartupPickerOutcome::Cancelled:
    case StartupPickerOutcome::Exited:
        // pi `selectSession` cancel → null (main.ts prints "No session
        // selected" and exits 0); the exit binding exits 0 silently — the
        // baseline component never fires onExit from handleInput (pi's
        // SessionList matches the same key set), so the two outcomes
        // converge here without an observable difference.
        return std::nullopt;
    }
    return std::unexpected(
            support::make_error(support::ErrorCode::Unknown, "startup session picker returned an unknown outcome"));
}

support::Expected<bool> run_process_terminal_missing_cwd_prompt(StartupTuiOptions options, std::string title) {
    return run_process_terminal_host<bool>(
            [options = std::move(options), title = std::move(title)](cch::tui::Terminal& terminal) mutable {
                return run_startup_missing_cwd_prompt(terminal, std::move(options), std::move(title));
            });
}

} // namespace cch::cli
