#pragma once

#include "../../../include/cch/agent/AgentEvent.hpp"
#include "../../../include/cch/ai/Content.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

[[nodiscard]] std::string text_from_content(const std::vector<ai::Content>& content);
void print_agent_event(const agent::AgentLifecycleEvent& event, std::ostream& out);

} // namespace cch::coding_agent::runtime
