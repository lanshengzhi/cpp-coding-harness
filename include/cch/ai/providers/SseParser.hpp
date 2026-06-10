#pragma once

#include <cch/util/Error.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai::providers {

struct SseEvent {
    std::string event{"message"};
    std::string data;
    bool done{false};
};

class SseParser {
public:
    [[nodiscard]] util::Expected<std::vector<SseEvent>> append(std::string_view bytes);
    [[nodiscard]] util::Expected<std::optional<SseEvent>> finish();
    void reset();

private:
    [[nodiscard]] util::Expected<std::optional<SseEvent>> consume_line(std::string line);
    [[nodiscard]] std::optional<SseEvent> dispatch_event();

    std::string pending_;
    std::string event_name_;
    std::vector<std::string> data_lines_;
};

} // namespace cch::ai::providers
