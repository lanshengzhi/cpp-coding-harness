#pragma once

#include "coding_agent/extensions/ExtensionToolSource.hpp"

#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::extensions {

/// How a native extension's `send_message` reaches a session that is already
/// running (pi `sendMessage` `deliverAs`: `"steer"` or `"followUp"`). From
/// idle the message always starts a run, the `triggerTurn: true` outcome.
enum class ExtensionMessageDelivery { Steer, FollowUp };

struct ExtensionSendMessageOptions {
    ExtensionMessageDelivery delivery{ExtensionMessageDelivery::Steer};
};

/// pi `BeforeAgentStartEvent` (native subset). `system_prompt` is the prompt
/// rendered at this point in the dispatch; a handler's replacement is what
/// later handlers observe and what the run starts with.
struct BeforeAgentStartEvent {
    enum class MessageType { User, Custom };

    MessageType message_type{MessageType::User};
    /// User trigger text after prompt-template/skill expansion (pi
    /// `event.prompt`).
    std::string prompt{};
    /// Images attached to the user trigger.
    std::vector<ai::ImageContent> images{};
    /// The custom message that triggered the run (`Custom` only).
    ai::CustomMessage message{};
    std::string system_prompt{};
};

/// pi `ExtensionContext` subset passed to `before_agent_start`.
struct ExtensionContext {
    /// pi `ctx.isIdle()`: false while the run that invoked the hook is being
    /// prepared, so the hook always observes a busy session.
    bool is_idle{true};
};

/// pi `BeforeAgentStartEventResult` (native subset): one custom message to
/// inject after the run's starting message, and an optional replacement for
/// this run's complete system prompt.
struct BeforeAgentStartResult {
    std::optional<ai::CustomMessage> message{std::nullopt};
    std::optional<std::string> system_prompt{std::nullopt};
};

/// pi `ExtensionAPI` action subset available to a native extension. The
/// session owns this handle for the extension's whole registration lifetime.
class ExtensionSession {
public:
    virtual ~ExtensionSession() = default;

    /// pi `ctx.isIdle()`.
    [[nodiscard]] virtual bool is_idle() const = 0;

    /// pi `sendMessage(message, { triggerTurn: true })`: from idle, append the
    /// custom message and start a run, completing when that run settles; while
    /// a run is active, queue it for the current run with `options.delivery`.
    [[nodiscard]] virtual support::AsyncResult<void> send_message(
            ai::CustomMessage message, ExtensionSendMessageOptions options = {}) = 0;
};

/// One compiled-in extension, the native counterpart of a pi extension
/// factory and its handler registrations. A native extension is also the
/// Extension Tool Source for its own tools, so `load_tools` joins the same
/// registry and execution path as every other source; the session binds
/// `bind` once the Agent exists, then dispatches `before_agent_start` before
/// every run.
///
/// Direct third-party pi TS/JS execution stays excluded: this surface is a C++
/// registration seam, not a plugin loader.
class NativeExtension : public ExtensionToolSource {
public:
    ~NativeExtension() override = default;

    /// Receive the session action handle (pi extension factory `pi` API).
    /// `session` outlives every extension registered with it.
    [[nodiscard]] virtual support::ExpectedVoid bind(ExtensionSession& session) = 0;

    /// pi `before_agent_start`: called in registration order while the run is
    /// busy, before the run's starting messages reach the model. A handler
    /// failure ends the run before the provider request.
    [[nodiscard]] virtual support::AsyncResult<BeforeAgentStartResult> before_agent_start(
            BeforeAgentStartEvent event, ExtensionContext context) = 0;
};

} // namespace cch::coding_agent::extensions
