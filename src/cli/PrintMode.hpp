#pragma once

#include "coding_agent/AgentSession.hpp"

#include <boost/asio/io_context.hpp>

#include <iosfwd>
#include <string>
#include <vector>

namespace cch::cli {

/// pi `runPrintMode` config for the C++ subset: the process streams the final
/// assistant text is printed to and terminal outcomes are reported on.
/// Borrowed streams; must outlive the `run_print_mode` call.
struct PrintModeConfig {
    std::ostream& output;
    std::ostream& error;
};

/// The print-mode prompt plan: the merged initial prompt (pi `initialMessage`)
/// plus the sequential prompts after it (pi `messages`).
struct PrintModePlan {
    std::string initial_message;
    std::vector<std::string> messages;
    coding_agent::PromptOptions initial_prompt_options{};
};

/// Run pi's print (single-shot) mode: prompt the initial message (with its
/// images) and then every remaining message sequentially, print only the
/// final assistant message's `text` content blocks to stdout, and report a
/// terminal `error`/`aborted` outcome's `errorMessage` (or `Request
/// <stopReason>`) on stderr with exit 1. Prompt preflight rejections report
/// `loop failed: <message>` on stderr with exit 1. SIGTERM/SIGHUP dispose the
/// session and exit 143/129. Running with no prompt
/// prints nothing and exits 0.
///
/// The session and the configured streams are borrowed and must outlive the
/// call; the run is driven synchronously on the CLI Runtime loop.
[[nodiscard]] int run_print_mode(
    boost::asio::io_context& io,
    coding_agent::AgentSession& session,
    PrintModeConfig config,
    PrintModePlan plan);

} // namespace cch::cli
