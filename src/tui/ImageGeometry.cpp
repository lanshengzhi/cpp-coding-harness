#include <cch/tui/TerminalImage.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cch::tui {
namespace {

[[nodiscard]] std::size_t choose_less_distorted_cell_count(std::size_t upper_count, long double ideal_count) {
    if (upper_count <= 1) return upper_count;
    const auto lower_count = upper_count - 1;
    const auto distortion = [ideal_count](std::size_t count) {
        const auto value = static_cast<long double>(count);
        return std::max(value / ideal_count, ideal_count / value);
    };
    return distortion(lower_count) < distortion(upper_count) ? lower_count : upper_count;
}

} // namespace

ImageCellSize calculate_image_cell_size(ImagePixelSize image_dimensions,
        std::size_t max_width_cells,
        std::optional<std::size_t> max_height_cells,
        CellPixelDimensions cell_dimensions,
        bool optimize_aspect_ratio) {
    const auto max_width = std::max<std::size_t>(1, max_width_cells);
    if (max_height_cells) *max_height_cells = std::max<std::size_t>(1, *max_height_cells);
    const auto image_width = std::max<std::size_t>(1, image_dimensions.width);
    const auto image_height = std::max<std::size_t>(1, image_dimensions.height);
    const auto cell_width = std::max<std::size_t>(1, cell_dimensions.width);
    const auto cell_height = std::max<std::size_t>(1, cell_dimensions.height);

    const auto width_scale = static_cast<long double>(max_width) * cell_width / image_width;
    const auto height_scale =
            max_height_cells ? static_cast<long double>(*max_height_cells) * cell_height / image_height : width_scale;
    const auto scale = std::min(width_scale, height_scale);
    const auto scaled_width = static_cast<long double>(image_width) * scale / cell_width;
    const auto scaled_height = static_cast<long double>(image_height) * scale / cell_height;
    auto columns = std::max<std::size_t>(1, std::min(max_width, static_cast<std::size_t>(std::ceil(scaled_width))));
    auto rows = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(scaled_height)));
    if (max_height_cells) rows = std::min(*max_height_cells, rows);

    if (optimize_aspect_ratio) {
        if (width_scale <= height_scale) {
            const auto ideal_rows = static_cast<long double>(columns) * cell_width * image_height /
                                    (static_cast<long double>(image_width) * cell_height);
            rows = choose_less_distorted_cell_count(rows, ideal_rows);
        } else {
            const auto ideal_columns = static_cast<long double>(rows) * cell_height * image_width /
                                       (static_cast<long double>(image_height) * cell_width);
            columns = choose_less_distorted_cell_count(columns, ideal_columns);
        }
    }
    return {.columns = columns, .rows = rows};
}

std::size_t calculate_image_rows(
        ImagePixelSize image_dimensions, std::size_t target_width_cells, CellPixelDimensions cell_dimensions) {
    return calculate_image_cell_size(image_dimensions, target_width_cells, std::nullopt, cell_dimensions).rows;
}

} // namespace cch::tui
