#pragma once

#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../../include/cch/harness/session/SessionResume.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct SessionOpenRequest {
    std::filesystem::path session_path;
    std::filesystem::path resume_path;
    std::filesystem::path workspace;
    bool workspace_explicit{false};
    std::string session_id;
    std::string created_at;
    std::string provider;
    std::string model;
};

struct OpenSession {
    std::filesystem::path workspace;
    harness::session::SessionMetadata metadata;
    std::vector<ai::MessageVariant> history;
    std::unique_ptr<harness::session::JsonlSessionStore> store;
    /// Provider stored in session metadata (populated on resume).
    std::optional<std::string> stored_provider;
    /// Model stored in session metadata (populated on resume).
    std::optional<std::string> stored_model;
    /// Effective model found on the active resume path.
    std::optional<std::string> context_model;
    /// Effective thinking level found on the active resume path.
    std::optional<std::string> context_thinking_level;
    /// Shape of the restored session context.
    harness::session::SessionTopology topology{harness::session::SessionTopology::Linear};
};

[[nodiscard]] util::Expected<OpenSession> open_session(SessionOpenRequest request);

} // namespace cch::coding_agent::runtime
