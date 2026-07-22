#pragma once

#include "../../include/cch/ai/Usage.hpp"
#include "../../third_party/catch2/catch_test_macros.hpp"

namespace cch::tests {

inline void check_usage(const ai::Usage& actual, const ai::Usage& expected) {
    CHECK(actual.input == expected.input);
    CHECK(actual.output == expected.output);
    CHECK(actual.cache_read == expected.cache_read);
    CHECK(actual.cache_write == expected.cache_write);
    CHECK(actual.cache_write_1h == expected.cache_write_1h);
    CHECK(actual.reasoning == expected.reasoning);
    CHECK(actual.total_tokens == expected.total_tokens);
    CHECK(actual.cost.input == expected.cost.input);
    CHECK(actual.cost.output == expected.cost.output);
    CHECK(actual.cost.cache_read == expected.cost.cache_read);
    CHECK(actual.cost.cache_write == expected.cost.cache_write);
    CHECK(actual.cost.total == expected.cost.total);
}

inline void check_zero_usage(const ai::Usage& usage) {
    check_usage(usage, ai::Usage{});
}

} // namespace cch::tests
