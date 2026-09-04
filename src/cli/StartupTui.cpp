#include "cli/StartupTui.hpp"

#include "coding_agent/SessionCwd.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/SessionSelector.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/ThemeController.hpp"
#include "support/ExpectedMacros.hpp"

#include <cch/support/Error.hpp>
#include <cch/tui/ProcessTerminal.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Tui.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace cch::cli {
namespace {

/// One awaiting startup-TUI outcome (pi `startup-ui.ts` promise): first
/// resolution wins, every callback hops to the consumer executor so the Tui
/// is only ever touched from one thread. Render requests are dropped once
/// the coroutine has finished tearing the host down.
template <typename T>
struct StartupSlot : std::enable_shared_from_this<StartupSlot<T>> {
    explicit StartupSlot(boost::asio::any_io_executor executor)
        : executor(std::move(executor)), channel(this->executor, 1) {}

    void resolve(support::Expected<T> value) {
        if (resolved.exchange(true)) return;
        const auto self = this->shared_from_this();
        boost::asio::post(
            executor, [self, value = std::move(value)]() mutable {
                self->channel.try_send(
                    boost::system::error_code{}, std::move(value));
            });
    }

    void request_render(cch::tui::Tui* tui) {
        const auto self = this->shared_from_this();
        boost::asio::post(executor, [self, tui] {
            if (!self->finished) (void)tui->render();
        });
    }

    boost::asio::any_io_executor executor;
    boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code, support::Expected<T>)>
        channel;
    std::atomic<bool> resolved{false};
    bool finished{false};
};

/// pi `createStartupTui` `setKeybindings(KeybindingsManager.create())`: the
/// startup registry reads <Agent Config Directory>/keybindings.json over the
/// assembled actions (the session-selector actions for the picker, the tui
/// builtins alone for the generic selector prompt).
[[nodiscard]] support::Expected<std::shared_ptr<const cch::tui::KeybindingRegistry>>
startup_keybindings(
    const StartupTuiOptions& options,
    std::span<const std::string_view> application_actions) {
    auto definitions = coding_agent::tui::app_keybinding_definitions(
        application_actions);
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
    const StartupTuiOptions& options,
    const cch::tui::Terminal& terminal) {
    auto resolved = coding_agent::tui::init_boot_theme(
        options.agent_config_directory, options.theme_setting);
    return cch::coding_agent::tui::LiveTheme{
        std::move(resolved), terminal.capabilities().color};
}

/// Run one startup-TUI coroutine on a fresh ProcessTerminal + io_context and
/// return its outcome (pi main.ts running `selectSession` /
/// `promptForMissingSessionCwd` before the main TUI on its own terminal).
template <typename T, typename Coroutine>
[[nodiscard]] support::Expected<T> run_process_terminal_host(Coroutine coroutine) {
    cch::tui::ProcessTerminal terminal;
    boost::asio::io_context io;
    auto future = boost::asio::co_spawn(
        io, std::move(coroutine)(terminal), boost::asio::use_future);
    io.run();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        auto result = future.get();
        if (!result) {
            return std::unexpected(result.error());
        }
        return std::move(*result);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "startup UI failed",
            error.what()));
    } catch (...) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "startup UI failed",
            "unknown exception"));
    }
#endif
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
[[nodiscard]] boost::asio::awaitable<support::Expected<T>> run_startup_host(
    cch::tui::Terminal& terminal,
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

    cch::tui::Tui tui{terminal};
    CCH_TRY_VOID(tui.start());
    // The detected capabilities (color depth, ...) are known only after the
    // terminal starts (pi `createStartupTui` constructs the palette after
    // the TUI is up; the controller default init renders at that depth).
    cch::coding_agent::tui::LiveTheme live_theme =
        start_live_theme(options, terminal);
    const auto request_render = [slot, &tui]() -> support::ExpectedVoid {
        slot->request_render(&tui);
        return {};
    };
    tui.set_render_request_sink(request_render);

    auto selector = build_selector(
        live_theme,
        *keybindings,
        slot,
        std::move_only_function<void()>{
            [slot, &tui] { slot->request_render(&tui); }});
    auto* selector_ptr = selector.get();
    CCH_TRY_VOID(tui.add_child(std::move(selector)));
    CCH_TRY_VOID(tui.set_focus(selector_ptr));
    CCH_TRY_VOID(tui.render());

    auto result = co_await slot->channel.async_receive(
        boost::asio::use_awaitable);

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

boost::asio::awaitable<support::Expected<StartupPickerResult>>
run_startup_session_picker(
    cch::tui::Terminal& terminal,
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
    return run_startup_host<StartupPickerResult>(
        terminal,
        std::move(options),
        kSessionActions,
        [current_loader = std::move(current_loader),
         all_loader = std::move(all_loader)](
            const cch::coding_agent::tui::LiveTheme& live_theme,
            const std::shared_ptr<const cch::tui::KeybindingRegistry>&
                keybindings,
            const std::shared_ptr<StartupSlot<StartupPickerResult>>& slot,
            std::move_only_function<void()> request_render) mutable
            -> std::unique_ptr<cch::tui::Component> {
            return std::make_unique<
                coding_agent::tui::SessionSelectorComponent>(
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
                [request_render = std::move(request_render)]() mutable {
                    (void)request_render();
                });
        });
}

boost::asio::awaitable<support::Expected<bool>> run_startup_missing_cwd_prompt(
    cch::tui::Terminal& terminal,
    StartupTuiOptions options,
    std::string title) {
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
                return std::make_unique<cch::tui::SelectList>(
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
                                .title = std::move(title),
                                .hint = coding_agent::tui::generic_select_list_hint(*keybindings),
                                .border_hook = live_theme.foreground_hook(cch::coding_agent::tui::ThemeToken::Border),
                        });
            });
}

support::Expected<std::optional<std::filesystem::path>>
run_process_terminal_resume_picker(
    StartupTuiOptions options,
    coding_agent::tui::SessionListLoader current_loader,
    coding_agent::tui::SessionListLoader all_loader) {
    auto result = run_process_terminal_host<StartupPickerResult>(
        [options = std::move(options),
         current_loader = std::move(current_loader),
         all_loader = std::move(all_loader)](
            cch::tui::Terminal& terminal) mutable {
            return run_startup_session_picker(
                terminal,
                std::move(options),
                std::move(current_loader),
                std::move(all_loader));
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
    return std::unexpected(support::make_error(
        support::ErrorCode::Unknown,
        "startup session picker returned an unknown outcome"));
}

support::Expected<bool> run_process_terminal_missing_cwd_prompt(
    StartupTuiOptions options,
    std::string title) {
    return run_process_terminal_host<bool>(
        [options = std::move(options), title = std::move(title)](
            cch::tui::Terminal& terminal) mutable {
            return run_startup_missing_cwd_prompt(
                terminal, std::move(options), std::move(title));
        });
}

} // namespace cch::cli
