#include <cch/coding_agent/McpUpstreamStatus.hpp>

namespace cch::coding_agent {

std::string_view to_string(McpUpstreamState state) noexcept {
    switch (state) {
    case McpUpstreamState::Pending:
        return "pending";
    case McpUpstreamState::Connected:
        return "connected";
    case McpUpstreamState::Failed:
        return "failed";
    case McpUpstreamState::NeedsAuth:
        return "needs_auth";
    case McpUpstreamState::Disabled:
        return "disabled";
    }
    return "pending";
}

} // namespace cch::coding_agent
