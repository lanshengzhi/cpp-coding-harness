#include "tui/UnicodeWidth.hpp"
#include <cch/tui/TerminalImage.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cch::tui {
namespace {

constexpr std::string_view kKittyPrefix{"\x1b_G"};
constexpr std::string_view kITerm2Prefix{"\x1b]1337;File="};

std::mutex g_metadata_mutex;
std::unordered_map<std::uint32_t, detail::ImagePlacementMetadata> g_image_metadata;

} // namespace

TerminalImageHandle allocate_image_id() {
    thread_local std::mt19937 generator{std::random_device{}()};
    thread_local std::uniform_int_distribution<std::uint32_t> distribution{1, 0xffffffffU};
    return {.value = distribution(generator)};
}

std::string delete_kitty_image(TerminalImageHandle handle) {
    return std::format("\x1b_Ga=d,d=I,i={},q=2\x1b\\", handle.value);
}

std::string delete_all_kitty_images() { return "\x1b_Ga=d,d=A,q=2\x1b\\"; }

std::string delete_all_kitty_placements() { return "\x1b_Ga=d,d=a,q=2\x1b\\"; }

bool is_image_line(std::string_view line) {
    return line.find(kKittyPrefix) != std::string_view::npos || line.find(kITerm2Prefix) != std::string_view::npos;
}

std::optional<KittyImageHeader> parse_kitty_image_header(std::string_view line) {
    const auto sequence_start = line.find(kKittyPrefix);
    if (sequence_start == std::string_view::npos) return std::nullopt;
    const auto params_start = sequence_start + kKittyPrefix.length();
    auto params_end = line.find(';', params_start);
    const auto st_end = line.find("\x1b\\", params_start);
    const auto bel_end = line.find('\x07', params_start);
    const auto terminator = std::min(st_end, bel_end);
    if (params_end == std::string_view::npos || (terminator != std::string_view::npos && terminator < params_end)) {
        params_end = terminator;
    }
    if (params_end == std::string_view::npos) return std::nullopt;

    KittyImageHeader result{};
    std::string_view params = line.substr(params_start, params_end - params_start);
    std::size_t start = 0;
    while (start < params.size()) {
        const auto comma = params.find(',', start);
        const auto param =
                params.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        const auto eq = param.find('=');
        if (eq != std::string_view::npos) {
            const auto key = param.substr(0, eq);
            const auto value_str = param.substr(eq + 1);
            std::uint64_t num_val = 0;
            const auto [ptr, ec] = std::from_chars(value_str.data(), value_str.data() + value_str.size(), num_val);
            if (ec == std::errc{} && ptr == value_str.data() + value_str.size() && num_val > 0 &&
                    num_val <= 0xffffffffULL) {
                if (key == "i") {
                    result.ids.push_back(static_cast<std::uint32_t>(num_val));
                } else if (key == "r") {
                    result.rows = static_cast<std::size_t>(num_val);
                }
            }
        }
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return result;
}

std::vector<std::uint32_t> extract_kitty_image_ids(std::string_view line) {
    auto header = parse_kitty_image_header(line);
    return header ? std::move(header->ids) : std::vector<std::uint32_t>{};
}

std::size_t extract_kitty_image_rows(std::string_view line) {
    auto header = parse_kitty_image_header(line);
    return header ? header->rows : 1;
}

std::size_t get_kitty_image_reserved_rows(
        const std::vector<std::string>& lines, std::size_t index, std::size_t max_index) {
    if (index >= lines.size()) return 1;
    const auto rows = extract_kitty_image_rows(lines[index]);
    if (rows <= 1) return 1;

    const auto max_rows = std::min({rows, max_index >= index ? max_index - index + 1 : 1, lines.size() - index});
    std::size_t reserved_rows = 1;
    while (reserved_rows < max_rows) {
        const auto& line = lines[index + reserved_rows];
        if (is_image_line(line) || detail::measure_visible_width(line).width > 0) break;
        ++reserved_rows;
    }
    return reserved_rows;
}

std::pair<std::ptrdiff_t, std::ptrdiff_t> expand_changed_range_for_kitty_images(std::ptrdiff_t first_changed,
        std::ptrdiff_t last_changed,
        const std::vector<std::string>& previous_lines,
        const std::vector<std::string>& new_lines) {
    auto expanded_first = first_changed;
    auto expanded_last = last_changed;
    const auto expand_for_lines = [&](const std::vector<std::string>& lines) {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (extract_kitty_image_ids(lines[i]).empty()) continue;
            const auto block_end = static_cast<std::ptrdiff_t>(
                    i + get_kitty_image_reserved_rows(lines, i, lines.empty() ? 0 : lines.size() - 1) - 1);
            const auto idx = static_cast<std::ptrdiff_t>(i);
            if (idx >= first_changed || (idx <= last_changed && block_end >= first_changed)) {
                expanded_first = std::min(expanded_first, idx);
                expanded_last = std::max(expanded_last, block_end);
            }
        }
    };
    expand_for_lines(previous_lines);
    expand_for_lines(new_lines);
    return {expanded_first, expanded_last};
}

std::string delete_kitty_images(const std::vector<std::uint32_t>& ids) {
    std::string result;
    for (const auto id : ids) {
        result += delete_kitty_image(TerminalImageHandle{.value = id});
    }
    return result;
}

std::string delete_changed_kitty_images(
        std::ptrdiff_t first_changed, std::ptrdiff_t last_changed, const std::vector<std::string>& previous_lines) {
    if (first_changed < 0 || last_changed < first_changed || previous_lines.empty()) return {};

    std::vector<std::uint32_t> ids;
    const auto max_line = std::min(static_cast<std::size_t>(last_changed), previous_lines.size() - 1);
    for (std::size_t i = static_cast<std::size_t>(first_changed); i <= max_line; ++i) {
        for (const auto id : extract_kitty_image_ids(previous_lines[i])) {
            if (std::ranges::find(ids, id) == ids.end()) {
                ids.push_back(id);
            }
        }
    }
    return delete_kitty_images(ids);
}

std::vector<std::uint32_t> collect_kitty_image_ids(const std::vector<std::string>& lines) {
    std::vector<std::uint32_t> ids;
    for (const auto& line : lines) {
        for (const auto id : extract_kitty_image_ids(line)) {
            if (std::ranges::find(ids, id) == ids.end()) {
                ids.push_back(id);
            }
        }
    }
    return ids;
}

namespace detail {

void register_image_metadata(TerminalImageHandle handle, ImagePlacementMetadata metadata) {
    std::lock_guard lock(g_metadata_mutex);
    g_image_metadata[handle.value] = std::move(metadata);
}

std::optional<ImagePlacementMetadata> lookup_image_metadata(TerminalImageHandle handle) {
    std::lock_guard lock(g_metadata_mutex);
    auto it = g_image_metadata.find(handle.value);
    if (it == g_image_metadata.end()) return std::nullopt;
    return it->second;
}

void unregister_image_metadata(TerminalImageHandle handle) {
    std::lock_guard lock(g_metadata_mutex);
    g_image_metadata.erase(handle.value);
}

void clear_image_metadata() {
    std::lock_guard lock(g_metadata_mutex);
    g_image_metadata.clear();
}

} // namespace detail

} // namespace cch::tui
