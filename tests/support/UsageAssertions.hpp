#pragma once

#include "../../include/cch/ai/Usage.hpp"
#include "../../third_party/catch2/catch_test_macros.hpp"

namespace cch::tests {

inline void check_zero_usage(const ai::Usage& usage) {
    CHECK(usage.input == 0);
    CHECK(usage.output == 0);
    CHECK(usage.cache_read == 0);
    CHECK(usage.cache_write == 0);
    CHECK_FALSE(usage.cache_write_1h.has_value());
    CHECK_FALSE(usage.reasoning.has_value());
    CHECK(usage.total_tokens == 0);
    CHECK(usage.cost.input == 0.0);
    CHECK(usage.cost.output == 0.0);
    CHECK(usage.cost.cache_read == 0.0);
    CHECK(usage.cost.cache_write == 0.0);
    CHECK(usage.cost.total == 0.0);
}

} // namespace cch::tests
