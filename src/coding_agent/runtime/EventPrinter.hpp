#pragma once

#include "../../../include/cch/agent/AgentEvent.hpp"

#include <iosfwd>

namespace cch::coding_agent::runtime {

void print_agent_event(const agent::AgentLifecycleEvent& event, std::ostream& out);

} // namespace cch::coding_agent::runtime
