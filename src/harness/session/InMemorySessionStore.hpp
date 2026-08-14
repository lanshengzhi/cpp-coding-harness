#pragma once

#include <cch/agent/harness/session/SessionStore.hpp>

#include <optional>

namespace cch::harness::session {

/// Runtime-facing no-persistence store for an in-memory Agent Session.
/// Live Session State remains owned exclusively by AgentSessionRuntime.
class InMemorySessionStore final : public SessionStore {
public:
    [[nodiscard]] support::ExpectedVoid append(const ai::MessageVariant& message) override;
    [[nodiscard]] std::optional<std::filesystem::path> path() const override;
};

} // namespace cch::harness::session
