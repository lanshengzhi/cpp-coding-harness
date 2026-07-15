#pragma once

#include "../../../include/cch/agent/AgentEvent.hpp"
#include "../../../include/cch/harness/session/SessionEntry.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <iosfwd>
#include <string>

namespace cch::coding_agent::runtime {

class JsonEventPrinter {
public:
    explicit JsonEventPrinter(std::ostream& out);

    [[nodiscard]] util::ExpectedVoid print_session_header(const harness::session::SessionMetadata& metadata);
    [[nodiscard]] util::ExpectedVoid print_agent_event(const agent::AgentLifecycleEvent& event);

private:
    [[nodiscard]] util::ExpectedVoid write_record(util::JsonValue::object_t record);

    std::ostream* out_;
};

} // namespace cch::coding_agent::runtime
