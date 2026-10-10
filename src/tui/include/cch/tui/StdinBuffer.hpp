#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

struct StdinBufferOptions {
    /// Maximum time to wait for an incomplete non-escape fragment (pi default 50 ms).
    std::chrono::milliseconds timeout{50};
    /// Maximum time to wait after a lone ESC before treating it as Escape (pi default 10 ms).
    std::chrono::milliseconds escape_timeout{10};
};

/// Reusable stdin framing seam equivalent to pi's StdinBuffer: split batched
/// terminal input into complete sequences before key/paste decoding.
class StdinBuffer final {
public:
    using DataHandler = std::function<void(std::string)>;
    using PasteHandler = std::function<void(std::string)>;

    explicit StdinBuffer(StdinBufferOptions options = {});

    void set_data_handler(DataHandler handler) { data_handler_ = std::move(handler); }
    void set_paste_handler(PasteHandler handler) { paste_handler_ = std::move(handler); }

    void process(std::string_view input);
    [[nodiscard]] std::vector<std::string> flush();
    void clear();
    void destroy() { clear(); }

    [[nodiscard]] std::string_view get_buffer() const noexcept { return buffer_; }
    [[nodiscard]] bool holds_fragment() const noexcept { return !buffer_.empty(); }
    [[nodiscard]] bool holds_lone_escape() const noexcept { return buffer_.size() == 1 && buffer_.front() == '\x1b'; }
    [[nodiscard]] std::chrono::milliseconds selected_timeout() const noexcept {
        return holds_lone_escape() ? options_.escape_timeout : options_.timeout;
    }
    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept { return deadline_; }

private:
    void emit_data_sequence(std::string sequence);
    void schedule_timeout();
    void cancel_timeout();

    StdinBufferOptions options_;
    DataHandler data_handler_;
    PasteHandler paste_handler_;
    std::string buffer_;
    bool paste_mode_{false};
    std::string paste_buffer_;
    std::optional<unsigned int> pending_kitty_printable_codepoint_;
    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
};

} // namespace cch::tui
