#pragma once

#include <cch/tui/Component.hpp>
#include <iterator>

namespace cch::coding_agent::tui {

inline void append_render_result(cch::tui::RenderResult& destination, cch::tui::RenderResult rendered) {
    const auto row_offset = destination.lines.size();
    destination.lines.insert(destination.lines.end(),
            std::make_move_iterator(rendered.lines.begin()),
            std::make_move_iterator(rendered.lines.end()));
    for (auto& image : rendered.images) {
        image.region.row += row_offset;
        destination.images.push_back(std::move(image));
    }
}

} // namespace cch::coding_agent::tui
