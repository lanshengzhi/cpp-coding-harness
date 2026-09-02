#pragma once

// Focused-test stand-in for the composition host's one `TuiActionSink`
// (ADR 0040, issue #461): every closed application-level Native TUI action
// is recorded by alternative, and `ReplaceSessionAction` is routed through a
// test-supplied session creator (pi `createRuntime`). The sink shares the
// recorded state, so a recorder may be a local of a boot helper while the
// asynchronous interactive run (and its sink) outlive that scope.
//
// Owner-local by design (CODING_STANDARDS §11.5): this exercises the private
// coding_agent Native TUI action seam and is used only by coding_agent TUI
// tests, so it stays beside them instead of widening `tests/support/` with a
// single-Owner helper.

#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui::testing {

/// `ReplaceSessionAction` handler used by focused interactive tests: creates
/// the replacement/boot session from the request. Absent in a recorder, the
/// action resolves with the unavailable-host error.
using TestSessionFactorySink = std::move_only_function<
    support::Expected<CreateAgentSessionResult>(runtime::AgentSessionCreationRequest)>;

/// A lightweight summary of a `ReplaceSessionAction` payload (the request
/// itself is move-only, so tests assert the owned facts it carried).
struct ReplaceSessionRecord {
    std::filesystem::path workspace;
    std::string target;
    bool no_skills{false};
};

/// Records every closed application-level action delivered through one
/// `TuiActionSink`, mirroring how the CLI composition host dispatches on the
/// closed action value.
struct ActionSinkRecorder {
    struct State {
        /// The session generation stamped on each delivered action, in
        /// delivery order (issue #461 generation seam).
        std::vector<std::size_t> generations;
        std::vector<OpenBrowserAction> open_browser;
        std::vector<WriteClipboardAction> write_clipboard;
        std::vector<SuspendProcessAction> suspend_process;
        std::vector<ReplaceSessionRecord> replace_sessions;
        std::vector<ReportBootDiagnosticsAction> boot_diagnostics;
        std::vector<ReportBootCreationFailureAction> boot_creation_failure;
        /// When set, creates the session for `ReplaceSessionAction`;
        /// otherwise the action resolves with the unavailable-host error.
        TestSessionFactorySink replace_session{nullptr};
        /// Asynchronous replacement creator used by tests migrated to the
        /// Session Assembly adapter. The generation and stop token are
        /// retained at this seam so cancellation and supersession remain
        /// observable without routing through the synchronous action value.
        AsyncSessionReplacementSink replace_session_async{nullptr};
        /// Number of asynchronous replacement results that completed at the
        /// seam (success or error). Tests waiting for a replacement to reach
        /// the engine pump until this counter advances, then drain: the
        /// engine's installation continuation is queued on the same loop by
        /// the time the count is observed.
        std::atomic<std::size_t> replacement_completions{0};
    };

    std::shared_ptr<State> state{std::make_shared<State>()};
    std::vector<std::size_t>& generations{state->generations};
    std::vector<OpenBrowserAction>& open_browser{state->open_browser};
    std::vector<WriteClipboardAction>& write_clipboard{state->write_clipboard};
    std::vector<SuspendProcessAction>& suspend_process{state->suspend_process};
    std::vector<ReplaceSessionRecord>& replace_sessions{state->replace_sessions};
    std::vector<ReportBootDiagnosticsAction>& boot_diagnostics{state->boot_diagnostics};
    std::vector<ReportBootCreationFailureAction>& boot_creation_failure{
        state->boot_creation_failure};
    TestSessionFactorySink& replace_session{state->replace_session};
    AsyncSessionReplacementSink& replace_session_async{state->replace_session_async};
    std::atomic<std::size_t>& replacement_completions{state->replacement_completions};

    /// One move-only sink carrying every recorded action (issue #461). The
    /// generation stamped on each action is recorded in `generations`.
    [[nodiscard]] TuiActionSink make_sink() {
        const auto shared = state;
        return [shared](std::size_t action_generation, TuiActionVariant action)
            -> support::Expected<TuiActionResultVariant> {
            shared->generations.push_back(action_generation);
            return std::visit(
                [shared](auto&& payload) -> support::Expected<TuiActionResultVariant> {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, OpenBrowserAction>) {
                        shared->open_browser.push_back(std::move(payload));
                        return TuiActionResultVariant{std::monostate{}};
                    } else if constexpr (std::is_same_v<T, WriteClipboardAction>) {
                        shared->write_clipboard.push_back(std::move(payload));
                        return TuiActionResultVariant{true};
                    } else if constexpr (std::is_same_v<T, SuspendProcessAction>) {
                        shared->suspend_process.push_back(std::move(payload));
                        return TuiActionResultVariant{std::monostate{}};
                    } else if constexpr (std::is_same_v<T, ReplaceSessionAction>) {
                        const std::string target = std::visit(
                            [](const auto& session_target) -> std::string {
                                using TT = std::decay_t<decltype(session_target)>;
                                if constexpr (std::is_same_v<TT, DefaultPersistedSessionTarget>) {
                                    return "default-persisted";
                                } else if constexpr (std::is_same_v<TT, ExplicitOpenOrCreateSessionTarget>) {
                                    return "explicit-open-or-create";
                                } else if constexpr (std::is_same_v<TT, ExplicitResumeSessionTarget>) {
                                    return "explicit-resume";
                                } else if constexpr (std::is_same_v<TT, ForkSessionTarget>) {
                                    return "fork";
                                } else if constexpr (std::is_same_v<TT, ContinueRecentSessionTarget>) {
                                    return "continue-recent";
                                } else {
                                    return "in-memory";
                                }
                            },
                            payload.request.session_target);
                        shared->replace_sessions.push_back(ReplaceSessionRecord{
                            .workspace = payload.request.workspace,
                            .target = target,
                            .no_skills = payload.request.session_facts.no_skills,
                        });
                        if (!shared->replace_session) {
                            return TuiActionResultVariant{
                                support::Expected<CreateAgentSessionResult>{std::unexpected(
                                    support::make_error(
                                        support::ErrorCode::Unknown,
                                        "no session creator installed"))}};
                        }
                        return TuiActionResultVariant{
                            shared->replace_session(std::move(payload.request))};
                    } else if constexpr (std::is_same_v<T, ReportBootDiagnosticsAction>) {
                        shared->boot_diagnostics.push_back(std::move(payload));
                        return TuiActionResultVariant{std::monostate{}};
                    } else {
                        shared->boot_creation_failure.push_back(std::move(payload));
                        return TuiActionResultVariant{std::monostate{}};
                    }
                },
                std::move(action));
        };
    }

    /// One move-only asynchronous replacement sink for tests that exercise
    /// the Session Assembly adapter directly. Replacement records use the
    /// same shared state as the closed action sink so either seam remains
    /// externally observable in one recorder.
    [[nodiscard]] AsyncSessionReplacementSink make_async_session_replacement_sink() {
        const auto shared = state;
        return [shared](std::size_t action_generation,
                       runtime::AgentSessionCreationRequest request,
                       std::stop_token stop_token) -> support::AsyncResult<CreateAgentSessionResult> {
            shared->generations.push_back(action_generation);
            const std::string target = std::visit(
                    [](const auto& session_target) -> std::string {
                        using T = std::decay_t<decltype(session_target)>;
                        if constexpr (std::is_same_v<T, DefaultPersistedSessionTarget>) {
                            return "default-persisted";
                        } else if constexpr (std::is_same_v<T, ExplicitOpenOrCreateSessionTarget>) {
                            return "explicit-open-or-create";
                        } else if constexpr (std::is_same_v<T, ExplicitResumeSessionTarget>) {
                            return "explicit-resume";
                        } else if constexpr (std::is_same_v<T, ForkSessionTarget>) {
                            return "fork";
                        } else if constexpr (std::is_same_v<T, ContinueRecentSessionTarget>) {
                            return "continue-recent";
                        } else {
                            return "in-memory";
                        }
                    },
                    request.session_target);
            shared->replace_sessions.push_back(ReplaceSessionRecord{
                    .workspace = request.workspace,
                    .target = target,
                    .no_skills = request.session_facts.no_skills,
            });
            if (!shared->replace_session_async) {
                shared->replacement_completions.fetch_add(1, std::memory_order_release);
                return support::AsyncResult<CreateAgentSessionResult>{std::unexpected(
                        support::make_error(support::ErrorCode::Unknown, "no asynchronous session creator installed"))};
            }
            auto inner = shared->replace_session_async(action_generation, std::move(request), stop_token);
            return support::detail::make_async_result(
                    [shared, inner = std::move(inner)]() mutable
                        -> boost::asio::awaitable<support::Expected<CreateAgentSessionResult>> {
                        auto outcome = co_await support::detail::await_async_result(std::move(inner));
                        shared->replacement_completions.fetch_add(1, std::memory_order_release);
                        co_return outcome;
                    });
        };
    }
};

} // namespace cch::coding_agent::tui::testing
