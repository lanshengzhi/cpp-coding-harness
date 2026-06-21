#include "ai/providers/SseParser.hpp"

#include <algorithm>

namespace cch::ai::providers {
namespace {

constexpr std::size_t max_pending_bytes = 8 * 1024 * 1024;

void strip_trailing_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

} // namespace

util::Expected<std::vector<SseEvent>> SseParser::append(std::string_view bytes) {
    if (pending_.size() + bytes.size() > max_pending_bytes) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Provider,
            "SSE stream buffer limit exceeded",
            "provider sent an unbounded SSE line"));
    }
    pending_.append(bytes.data(), bytes.size());

    std::vector<SseEvent> events;
    for (;;) {
        const auto newline = pending_.find('\n');
        if (newline == std::string::npos) {
            break;
        }

        auto line = pending_.substr(0, newline);
        pending_.erase(0, newline + 1);
        strip_trailing_cr(line);

        auto event = consume_line(std::move(line));
        if (!event) {
            return std::unexpected(event.error());
        }
        if (*event) {
            events.push_back(std::move(**event));
        }
    }

    return events;
}

util::Expected<std::optional<SseEvent>> SseParser::finish() {
    if (!pending_.empty()) {
        auto line = std::move(pending_);
        pending_.clear();
        strip_trailing_cr(line);
        auto event = consume_line(std::move(line));
        if (!event) {
            return std::unexpected(event.error());
        }
        if (*event) {
            return std::move(*event);
        }
    }

    return dispatch_event();
}

void SseParser::reset() {
    pending_.clear();
    event_name_.clear();
    data_lines_.clear();
}

util::Expected<std::optional<SseEvent>> SseParser::consume_line(std::string line) {
    if (line.empty()) {
        return dispatch_event();
    }

    if (line.front() == ':') {
        return std::optional<SseEvent>{};
    }

    const auto colon = line.find(':');
    const auto field = colon == std::string::npos ? line : line.substr(0, colon);
    auto value = colon == std::string::npos ? std::string{} : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
    }

    if (field == "event") {
        event_name_ = std::move(value);
    } else if (field == "data") {
        data_lines_.push_back(std::move(value));
    }

    return std::optional<SseEvent>{};
}

std::optional<SseEvent> SseParser::dispatch_event() {
    if (event_name_.empty() && data_lines_.empty()) {
        return std::nullopt;
    }

    SseEvent event;
    event.event = event_name_.empty() ? "message" : event_name_;
    for (std::size_t index = 0; index < data_lines_.size(); ++index) {
        if (index != 0) {
            event.data.push_back('\n');
        }
        event.data += data_lines_[index];
    }
    event.done = event.data == "[DONE]";

    event_name_.clear();
    data_lines_.clear();
    return event;
}

} // namespace cch::ai::providers
