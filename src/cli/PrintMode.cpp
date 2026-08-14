#include "PrintMode.hpp"

#include "coding_agent/BoundedText.hpp"
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_future.hpp>

#include <csignal>
#include <exception>
#include <format>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::cli {
namespace {

/// The one stderr diagnostic for a terminal error/aborted outcome, exactly
/// like pi `runPrintMode`: the assistant's `errorMessage`, or the
/// `Request <stopReason>` fallback. Redaction and the bounded-output policy
/// are the C++ binary's own presentation guardrails.
[[nodiscard]] std::string terminal_diagnostic(
    const ai::AssistantMessage& assistant) {
    if (assistant.error_message && !assistant.error_message->empty()) {
        return coding_agent::bounded_redacted_presentation(*assistant.error_message);
    }
    return std::format(
        "Request {}", ai::stop_reason_to_string(assistant.stop_reason));
}

/// pi `runPrintMode` text mode: only the final assistant message's `text`
/// content blocks reach stdout. Returns false when the final message is a
/// terminal error/aborted outcome (already reported on stderr with exit 1).
[[nodiscard]] bool print_final_assistant_text(
    const coding_agent::AgentSessionSnapshot& snapshot,
    PrintModeConfig config) {
    const auto& messages = snapshot.agent_state.messages;
    if (messages.empty()) {
        return true;
    }
    const auto* assistant = std::get_if<ai::AssistantMessage>(&messages.back());
    if (assistant == nullptr) {
        return true;
    }

    if (assistant->stop_reason == ai::AssistantStopReason::Error ||
        assistant->stop_reason == ai::AssistantStopReason::Aborted) {
        config.error << terminal_diagnostic(*assistant) << '\n';
        config.error.flush();
        return false;
    }
    for (const auto& content : assistant->content) {
        if (const auto* text = std::get_if<ai::TextContent>(&content)) {
            config.output << coding_agent::bounded_redacted_presentation(text->text) << '\n';
        }
    }
    config.output.flush();
    return true;
}

/// The print-mode coroutine: pi `runPrintMode` on the awaiting executor, with
/// pi's signal handlers registered as an Asio signal set so a signal disposes
/// the session (abort the active prompt, close) and exits 143/129. The
/// session and the configured streams are borrowed; must outlive the
/// coroutine.
[[nodiscard]] boost::asio::awaitable<int> run_print_mode_coro(
    coding_agent::AgentSession& session,
    PrintModeConfig config,
    PrintModePlan plan) {
    const auto executor = co_await boost::asio::this_coro::executor;

    // pi `registerSignalHandlers`: SIGTERM always, SIGHUP outside Windows.
    // The handler runs on this executor (single-threaded with the prompts),
    // disposes the session like pi, and records the exit code.
    boost::asio::signal_set signals{executor, SIGTERM};
#if !defined(_WIN32)
    signals.add(SIGHUP);
#endif
    std::optional<int> signal_exit;
    signals.async_wait([&](const boost::system::error_code& error, int fired) {
        if (error) return;
#if defined(_WIN32)
        (void)fired;
        signal_exit = 143;
#else
        signal_exit = fired == SIGHUP ? 129 : 143;
#endif
        // pi print-mode handler: dispose the session before exiting.
        session.abort();
        session.close();
    });

    // Settles one prompt: a signal preempts everything (pi exits inside the
    // handler), then a prompt rejection keeps the C++ binary's loop-failed
    // report with a non-zero exit.
    const auto settle = [&](util::ExpectedVoid prompted)
        -> std::optional<int> {
        if (signal_exit) {
            return *signal_exit;
        }
        if (!prompted) {
            config.error << "loop failed: " << prompted.error().message << '\n';
            config.error.flush();
            return 1;
        }
        return std::nullopt;
    };

    int exit_code = 0;
    bool settled = false;

    if (!plan.initial_message.empty() || !plan.initial_prompt_options.images.empty()) {
        auto prompted = co_await session.prompt(
            std::move(plan.initial_message),
            std::move(plan.initial_prompt_options));
        if (auto failed = settle(std::move(prompted))) {
            exit_code = *failed;
            settled = true;
        }
    }

    // pi: `for (const message of messages) await session.prompt(message)`.
    for (auto& message : plan.messages) {
        if (settled) break;
        auto prompted = co_await session.prompt(std::move(message));
        if (auto failed = settle(std::move(prompted))) {
            exit_code = *failed;
            settled = true;
        }
    }

    if (!settled) {
        if (signal_exit) {
            exit_code = *signal_exit;
        } else {
            // pi text mode: the last message determines the outcome.
            const auto snapshot = session.snapshot();
            if (!print_final_assistant_text(snapshot, config)) {
                exit_code = 1;
            }
        }
    }

    // The pending signal wait keeps the io_context alive; cancel it so the
    // run drains (the cancellation completes the handler with an error).
    boost::system::error_code ignored;
    signals.cancel(ignored);
    co_return exit_code;
}

} // namespace

int run_print_mode(
    boost::asio::io_context& io,
    coding_agent::AgentSession& session,
    PrintModeConfig config,
    PrintModePlan plan) {
    auto future = boost::asio::co_spawn(
        io,
        run_print_mode_coro(session, config, std::move(plan)),
        boost::asio::use_future);
    while (future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        (void)io.run_one();
    }
    try {
        return future.get();
    } catch (const std::exception& error) {
        config.error << "print mode failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        config.error << "print mode failed: unknown error\n";
        return 1;
    }
}

} // namespace cch::cli
