#pragma once

#include <cch/tui/Input.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace cch::tui::detail {

class InputDecoder final {
public:
    [[nodiscard]] std::vector<InputEventVariant> feed(std::string_view input);
    [[nodiscard]] std::vector<InputEventVariant> flush();
    void reset();

private:
    enum class EscapeDiscardMode {
        None,
        Csi,
        Osc,
        StringTerminated,
    };

    void drain(std::vector<InputEventVariant>& events, bool end_of_feed);
    [[nodiscard]] bool discard_escape_byte(char byte);
    void enter_paste();
    void consume_paste_byte(char byte, std::vector<InputEventVariant>& events);
    void commit_paste_byte(char byte);

    std::string pending_;
    EscapeDiscardMode discard_mode_{EscapeDiscardMode::None};
    bool discard_saw_escape_{false};
    bool paste_mode_{false};
    std::string paste_text_;
    std::string paste_end_candidate_;
    std::size_t paste_original_bytes_{0};
    std::size_t paste_lines_{1};
};

} // namespace cch::tui::detail
