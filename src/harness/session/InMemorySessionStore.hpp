#pragma once

#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <optional>

namespace cch::harness::session {

/// The in-memory closed alternative behind the SessionStore facade: a
/// no-persistence store for an in-memory Agent Session. Live Session State
/// remains owned exclusively by AgentSessionRuntime.
class InMemorySessionStore final {
public:
    [[nodiscard]] support::ExpectedVoid append(const ai::MessageVariant& message);
    [[nodiscard]] std::optional<std::filesystem::path> path() const;
};

} // namespace cch::harness::session
