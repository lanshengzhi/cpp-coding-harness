#pragma once

#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/harness/session/JsonlSessionStore.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <filesystem>
#include <memory>
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
    std::vector<ai::MessageVariant> history;
    std::unique_ptr<harness::session::JsonlSessionStore> store;
    /// Provider stored in session metadata (populated on resume).
    std::optional<std::string> stored_provider;
    /// Model stored in session metadata (populated on resume).
    std::optional<std::string> stored_model;
};

[[nodiscard]] util::Expected<OpenSession> open_session(SessionOpenRequest request);

} // namespace cch::coding_agent::runtime
