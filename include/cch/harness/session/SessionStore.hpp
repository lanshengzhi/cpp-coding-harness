#pragma once

#include "../../ai/Message.hpp"
#include "../../util/Error.hpp"

#include <filesystem>

namespace cch::harness::session {

/// Runtime-facing persistence capability for an Agent Session.
///
/// Concrete stores may expose additional loading or navigation operations, but
/// Runtime depends only on incremental message append and path introspection.
class SessionStore {
public:
    virtual ~SessionStore() = default;

    [[nodiscard]] virtual util::ExpectedVoid append(const ai::MessageVariant& message) = 0;
    [[nodiscard]] virtual const std::filesystem::path& path() const = 0;
};

} // namespace cch::harness::session
