#include "mcp/SseResponseStream.hpp"

#include <cch/support/Error.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cch::mcp::sse {
namespace {

using support::Error;
using support::ErrorCode;
using support::Expected;
using support::ExpectedVoid;
using support::make_error;

/// The one field this build acts on; every other line — a comment, a `retry:`,
/// an `id:` — is skipped by not matching this prefix.
constexpr std::string_view kDataField{"data:"};

/// Strip the optional single space a field value may carry after its colon.
[[nodiscard]] std::string_view field_value(std::string_view line) {
    const auto value = line.substr(kDataField.size());
    if (value.starts_with(' ')) {
        return value.substr(1);
    }
    return value;
}

} // namespace

ExpectedVoid SseResponseStream::append(std::string_view bytes) {
    pending_.append(bytes);
    for (;;) {
        const auto newline = pending_.find('\n');
        if (newline == std::string::npos) {
            return {};
        }
        auto line = std::string_view{pending_}.substr(0, newline);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        // The line is consumed before the erase: it is a view into `pending_`,
        // and erasing first would move the bytes out from under it.
        const bool accepted = consume_line(line);
        pending_.erase(0, newline + 1);
        if (!accepted) {
            return std::unexpected(flooded_error());
        }
    }
}

Expected<std::string> SseResponseStream::finish() const {
    if (frame_open_ || !pending_.empty()) {
        return std::unexpected(make_error(ErrorCode::Network,
                "the Upstream MCP Server response stream broke before the frame it was sending completed",
                "the 2026-07-28 revision has no Mcp-Session-Id and no SSE resumability, so the in-flight request "
                "is lost rather than resumable"));
    }
    return body_;
}

Error SseResponseStream::flooded_error() const {
    return make_error(ErrorCode::ResourceLimit,
            "the Upstream MCP Server response passed the MCP Host's retention bound",
            "the response is terminated rather than drained, so a flooding Upstream cannot hold the connection "
            "open or stall another call");
}

bool SseResponseStream::consume_line(std::string_view line) {
    if (line.empty()) {
        return dispatch_frame();
    }
    if (!line.starts_with(kDataField)) {
        return true; // a comment, or a field this build does not act on
    }
    frame_open_ = true;
    frame_data_.append(field_value(line));
    frame_data_.push_back('\n');
    return true;
}

bool SseResponseStream::dispatch_frame() {
    if (!frame_open_) {
        return true;
    }
    frame_open_ = false;
    // The payload lines of one frame join with newlines, and the newline the
    // assembler appends after the last of them is not part of its data.
    frame_data_.pop_back();
    if (body_.size() + frame_data_.size() > max_bytes_) {
        return false;
    }
    body_ += frame_data_;
    frame_data_.clear();
    return true;
}

} // namespace cch::mcp::sse
