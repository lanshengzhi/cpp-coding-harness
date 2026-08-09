#pragma once

#include <cstddef>
#include <cstdint>

namespace cch::coding_agent::runtime {

/// pi `getSessionStats` subset (`agent-session.ts` `SessionStats`): the
/// per-role message counts and usage/token totals the `/session` command
/// renders. Aggregated over the session history (persisted sessions read
/// the file's entries so compacted-away history still counts, like pi).
struct SessionStats {
    std::size_t total_messages{0};
    std::size_t user_messages{0};
    std::size_t assistant_messages{0};
    std::size_t tool_calls{0};
    std::size_t tool_results{0};
    std::int64_t input_tokens{0};
    std::int64_t output_tokens{0};
    std::int64_t cache_read{0};
    std::int64_t cache_write{0};
};

} // namespace cch::coding_agent::runtime
