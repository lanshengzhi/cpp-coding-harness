#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace cch::mcp::sse {

/// Assembles the body of a Streamable HTTP reply that arrives as a
/// `text/event-stream`, which is how an Upstream streams progress and server
/// notifications ahead of the response its request is waiting for.
///
/// The assembler keeps only what the client stack reads — the concatenated
/// payload text of the frames it has completed — and is deliberately narrower
/// than a general event-stream reader: `event:`, `retry:`, and `id:` fields
/// are ignored outright. The 2026-07-28 revision removed SSE resumability, so
/// a frame id has nothing to resume from and the transport never sends
/// `Last-Event-ID` (ADR 0064).
///
/// The retention bound is this type's own: a frame that would push the
/// assembled body past `max_bytes` is refused rather than kept, which is what
/// lets a flooding Upstream be terminated instead of drained (spec #833 story
/// 24).
class SseResponseStream {
public:
    explicit SseResponseStream(std::size_t max_bytes) noexcept : max_bytes_(max_bytes) {}

    /// Feed response bytes as they arrive. A payload that would pass
    /// `max_bytes` fails the append; the caller terminates the exchange and
    /// the partial body is released with it.
    [[nodiscard]] cch::support::ExpectedVoid append(std::string_view bytes);

    /// The assembled payload text. A stream that ended in the middle of a
    /// frame fails: its partial frame is not a response, and with no
    /// `Mcp-Session-Id` and no resumability a broken stream loses the
    /// in-flight request rather than leaving it resumable.
    [[nodiscard]] cch::support::Expected<std::string> finish() const;

private:
    [[nodiscard]] bool consume_line(std::string_view line);
    [[nodiscard]] bool dispatch_frame();
    [[nodiscard]] cch::support::Error flooded_error() const;

    /// Bytes of a line the response has not completed yet.
    std::string pending_{};
    /// The payload lines of the frame being assembled, joined by a newline.
    std::string frame_data_{};
    /// Whether a frame has been started but not yet dispatched.
    bool frame_open_{false};
    std::string body_{};
    std::size_t max_bytes_{0};
};

} // namespace cch::mcp::sse
