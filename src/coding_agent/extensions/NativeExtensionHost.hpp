#pragma once

#include "coding_agent/extensions/NativeExtension.hpp"

#include <cch/ai/Message.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::extensions {

/// Combined pi `before_agent_start` result across the registered extensions:
/// the messages to inject in registration order and the last system-prompt
/// replacement a handler returned.
struct BeforeAgentStartOutcome {
    std::vector<ai::CustomMessage> messages{};
    std::optional<std::string> system_prompt{std::nullopt};
};

/// Owns the session's native extension registrations and the one dispatch
/// order (pi `ExtensionRunner` `emitBeforeAgentStart`, native subset). The
/// registry is immutable after construction, so the asynchronous dispatch owns
/// a shared snapshot and never borrows the session.
class NativeExtensionHost {
public:
    NativeExtensionHost();
    explicit NativeExtensionHost(std::vector<std::shared_ptr<NativeExtension>> extensions);

    [[nodiscard]] bool empty() const noexcept;

    /// Bind every extension in registration order (pi extension factory load).
    [[nodiscard]] support::ExpectedVoid bind(ExtensionSession& session) const;

    /// Dispatch `before_agent_start` in registration order. Each handler sees
    /// the prompt replacement returned by the handlers before it; a handler
    /// failure stops the dispatch and is returned unchanged.
    [[nodiscard]] support::AsyncResult<BeforeAgentStartOutcome> before_agent_start(
            BeforeAgentStartEvent event, ExtensionContext context) const;

private:
    /// Shared so a pending dispatch never borrows the host object.
    std::shared_ptr<const std::vector<std::shared_ptr<NativeExtension>>> extensions_;
};

} // namespace cch::coding_agent::extensions
