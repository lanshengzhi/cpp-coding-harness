#pragma once

#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>

#include <filesystem>
#include <optional>

namespace cch::harness::session {

/// Runtime-facing persistence capability for an Agent Session.
///
/// Concrete stores may expose additional loading or navigation operations, but
/// Runtime depends only on incremental message append and path introspection.
class SessionStore {
public:
    virtual ~SessionStore() = default;

    [[nodiscard]] virtual support::ExpectedVoid append(const ai::MessageVariant& message) = 0;
    [[nodiscard]] virtual std::optional<std::filesystem::path> path() const = 0;
};

} // namespace cch::harness::session
