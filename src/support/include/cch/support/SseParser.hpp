#pragma once

#include <cch/support/Error.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::support {

struct SseEvent {
    std::string event{"message"};
    std::string data;
    bool done{false};
};

class SseParser {
public:
    [[nodiscard]] Expected<std::vector<SseEvent>> append(std::string_view bytes);
    [[nodiscard]] Expected<std::optional<SseEvent>> finish();
    void reset();

private:
    [[nodiscard]] Expected<std::optional<SseEvent>> consume_line(std::string line);
    [[nodiscard]] std::optional<SseEvent> dispatch_event();

    std::string pending_;
    std::string event_name_;
    std::vector<std::string> data_lines_;
};

} // namespace cch::support
