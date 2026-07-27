#pragma once

#include <cch/tui/Terminal.hpp>
#include <cch/util/Error.hpp>

#include <string>
#include <string_view>

namespace cch::tui::detail {

[[nodiscard]] bool cell_regions_intersect(
    const CellRegion& left,
    const CellRegion& right);
[[nodiscard]] bool protocol_supports_mime(
    InlineImageProtocol protocol,
    std::string_view mime_type);

[[nodiscard]] util::Expected<std::string> encode_terminal_image(
    InlineImageProtocol protocol,
    const TerminalImage& image,
    TerminalImageHandle handle);

[[nodiscard]] util::Expected<std::string> encode_terminal_image_removal(
    InlineImageProtocol protocol,
    TerminalImageHandle handle);

} // namespace cch::tui::detail
