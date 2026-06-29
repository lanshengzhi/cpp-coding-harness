#pragma once

#include "../../include/cch/coding_agent/Sdk.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"

namespace cch::coding_agent {

[[nodiscard]] util::Expected<CreateAgentSessionResult> create_agent_session(
    runtime::AgentSessionCreationRequest request);

} // namespace cch::coding_agent
