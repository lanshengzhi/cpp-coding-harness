#pragma once

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

struct ClipboardImage {
    std::vector<std::uint8_t> bytes;
    std::string mime_type;
};

class AsyncClipboardReader {
public:
    virtual ~AsyncClipboardReader() = default;

    [[nodiscard]] virtual boost::asio::awaitable<support::Expected<std::optional<ClipboardImage>>>
    read_image() = 0;
    [[nodiscard]] virtual boost::asio::awaitable<support::Expected<std::optional<std::string>>>
    read_text() = 0;
};

} // namespace cch::coding_agent::tui
