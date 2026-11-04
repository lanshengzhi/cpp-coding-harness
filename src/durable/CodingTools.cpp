#include "durable/EditDiff.hpp"
#include "durable/FileMutationQueue.hpp"
#include "durable/ToolContent.hpp"
#include "Utf8Text.hpp"

#include <cch/durable/Environment.hpp>
#include <cch/durable/Images.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/OutputLimiter.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::durable {
namespace {
using Result = support::Expected<std::vector<ai::Content>>;
using PreparedResult = support::Expected<PreparedImage>;

support::Error tool_error(std::string text) { return support::make_error(support::ErrorCode::Tool, std::move(text)); }
support::Error image_error(std::string text) {
    return tool_error(std::format("<harness>\n[error] {}\n</harness>", text));
}
Result failure(std::string text) { return std::unexpected(tool_error(std::move(text))); }
Result text_result(std::string text) { return std::vector<ai::Content>{ai::text_content(std::move(text))}; }

/// An image the read tool found in a file: its path for diagnostics, its
/// bytes and their detected MIME type.
struct ReadImage {
    std::string_view path;
    std::span<const std::uint8_t> bytes;
    std::string_view mime_type;
};

PreparedResult prepare_with_processor(const ReadImage& image, const CodingToolOptions& options) {
    auto prepared = options.images->prepare(image.bytes, image.mime_type, options.limits);
    if (!prepared) {
        return std::unexpected(image_error(
                std::format("{} is an image ({}) that cannot be prepared for the model", image.path, image.mime_type)));
    }
    return std::move(*prepared);
}

PreparedResult prepare_without_processor(const ReadImage& image, const CodingToolOptions& options) {
    if (!is_inline_image_type(image.mime_type)) {
        return std::unexpected(image_error(
                std::format("{} is an image ({}) that needs converting, and no image processor is configured",
                        image.path,
                        image.mime_type)));
    }
    if (base64_encoded_length(image.bytes.size()) > options.limits.max_base64_bytes) {
        return std::unexpected(image_error(std::format("{} is an image ({}) of {}, too large to send, "
                                                       "and no image processor is configured to shrink it",
                image.path,
                image.mime_type,
                support::format_output_size(image.bytes.size()))));
    }
    const auto dimensions = image_dimensions(image.bytes, image.mime_type);
    if (!dimensions) {
        return std::unexpected(
                image_error(std::format("{} is an image ({}) whose size cannot be read", image.path, image.mime_type)));
    }
    if (dimensions->width > options.limits.max_width || dimensions->height > options.limits.max_height) {
        return std::unexpected(image_error(std::format("{} is an image ({}) of {}x{}, larger than {}x{}, "
                                                       "and no image processor is configured to shrink it",
                image.path,
                image.mime_type,
                dimensions->width,
                dimensions->height,
                options.limits.max_width,
                options.limits.max_height)));
    }
    return PreparedImage{.data = encode_base64(image.bytes), .mime_type = std::string{image.mime_type}};
}

std::vector<std::string> image_notes(const PreparedImage& image, bool vision) {
    std::vector<std::string> notes;
    if (image.converted_from)
        notes.push_back(std::format("Converted from {} to {}.", *image.converted_from, image.mime_type));
    if (image.resized) {
        const auto& [from, to] = *image.resized;
        notes.push_back(
                std::format("Resized from {}x{} to {}x{}. Multiply coordinates by {:.2f} to map them to the original.",
                        from.width,
                        from.height,
                        to.width,
                        to.height,
                        static_cast<double>(from.width) / static_cast<double>(to.width)));
    }
    if (!vision) notes.emplace_back("The current model does not support images; it sees a placeholder instead.");
    return notes;
}

std::vector<ai::Content> image_result(PreparedImage image, bool vision) {
    std::string message = std::format("Read image file [{}].", image.mime_type);
    for (const auto& note : image_notes(image, vision))
        message += std::format(" {}", note);
    return {ai::image_content(std::move(image.data), image.mime_type),
            ai::text_content(std::format("<harness>\n[info] {}\n</harness>", message))};
}

Result read_path(std::string_view path, Environment& env, const CodingToolOptions& options, std::stop_token stop) {
    auto content = env.read(path, stop);
    if (!content) return std::unexpected(content.error());
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(content->data()), content->size()};
    const auto mime_type = detect_image_mime_type(bytes);
    if (!mime_type) return text_result(std::move(*content));

    const ReadImage image{.path = path, .bytes = bytes, .mime_type = *mime_type};
    auto prepared = options.images ? prepare_with_processor(image, options) : prepare_without_processor(image, options);
    if (!prepared) return std::unexpected(prepared.error());
    return image_result(std::move(*prepared), options.vision);
}
const support::JsonValue* field(const support::JsonValue& value, std::string_view key) {
    const auto* object = value.get_if<support::JsonValue::object_t>();
    if (!object) return nullptr;
    auto found = object->find(std::string{key});
    return found == object->end() ? nullptr : &found->second;
}
const std::string* string_field(const support::JsonValue& value, std::string_view key) {
    auto* item = field(value, key);
    return item ? item->get_if<std::string>() : nullptr;
}

// The frozen read tool's bounded output (pi `truncate.ts`): the head is
// truncated at 2000 lines or 50 KiB, whichever binds first, never cutting a
// line or a UTF-8 character.
constexpr std::size_t kReadToolMaxLines = 2000;
constexpr std::size_t kReadToolMaxBytes = 50 * 1024;
constexpr std::size_t kReadChunkBytes = 64 * 1024;

/// `Array.prototype.slice`'s index conversion (frozen `sliceIndex`):
/// NaN is 0, every other value truncates toward zero.
double slice_index(double value) {
    if (std::isnan(value)) return 0;
    return value < 0 ? std::ceil(value) : std::floor(value);
}

bool is_safe_integer(double value) {
    return std::isfinite(value) && std::floor(value) == value && std::abs(value) <= 9'007'199'254'740'991.0;
}

/// JavaScript's Number-to-String rendering for the read tool diagnostics
/// (shortest round-trip, integers without a fraction).
std::string js_number(double value) {
    if (value == 0) return "0";
    char buffer[64]{};
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) return std::format("{}", value);
    return std::string(buffer, end);
}

/// Frozen `formatSize`: bytes below 1 KiB render as `NB`, below 1 MiB as one
/// decimal kilobyte, otherwise one decimal megabyte.
std::string format_size(std::size_t bytes) {
    char buffer[32]{};
    if (bytes < 1024) return std::format("{}B", bytes);
    if (bytes < 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(bytes) / 1024.0);
        return std::string(buffer) + "KB";
    }
    std::snprintf(buffer, sizeof(buffer), "%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return std::string(buffer) + "MB";
}

/// Where one file's lines are, as the frozen `LineScanner` finds them in one
/// streaming pass over the bytes from position 0. Decoded sizes are those of
/// `TextDecoder` output for the range, so a leading byte-order mark is not
/// counted; a trailing newline adds no line.
struct LineScan {
    std::uintmax_t newlines{};
    std::uintmax_t start{};
    std::uintmax_t end{};
    std::uintmax_t first_line_end{};
    std::uintmax_t last_line_start{};
    std::size_t selected_bytes{};
    std::size_t first_line_bytes{};
};

class LineScanner {
public:
    LineScanner(std::uintmax_t start_line, std::uintmax_t end_line) : start_line_(start_line), end_line_(end_line) {
        if (start_line == 0) begin(0);
    }

    void push(std::string_view chunk) {
        if (head_.has_value()) {
            const std::size_t take = std::min(3 - head_->size(), chunk.size());
            head_->append(chunk.substr(0, take));
            if (head_->size() < 3) return;
            release_head();
            chunk.remove_prefix(take);
        }
        process(chunk);
    }

    LineScan finish() {
        if (head_.has_value()) release_head();
        const std::uintmax_t size = position_;
        if (!start_.has_value())
            return LineScan{.newlines = newlines_,
                    .start = size,
                    .end = size,
                    .first_line_end = size,
                    .last_line_start = size,
                    .selected_bytes = 0,
                    .first_line_bytes = 0};
        if (!first_line_end_.has_value()) end_first_line(size);
        if (!end_.has_value()) end_selection(size);
        return LineScan{.newlines = newlines_,
                .start = *start_,
                .end = end_.value_or(size),
                .first_line_end = first_line_end_.value_or(size),
                .last_line_start = last_line_start_.value_or(line_start_),
                .selected_bytes = selected_bytes_,
                .first_line_bytes = first_line_bytes_};
    }

private:
    void release_head() {
        std::string head = std::move(*head_);
        head_ = std::nullopt;
        bom_ = head.size() >= 3 && static_cast<unsigned char>(head[0]) == 0xef &&
               static_cast<unsigned char>(head[1]) == 0xbb && static_cast<unsigned char>(head[2]) == 0xbf;
        process(head);
    }

    void process(std::string_view chunk) {
        const std::uintmax_t base = position_;
        std::size_t from = 0;
        for (std::size_t index = chunk.find('\n'); index != std::string_view::npos;
                index = chunk.find('\n', index + 1)) {
            feed(chunk, base, from, index);
            const std::uintmax_t line = newlines_;
            const std::uintmax_t position = base + index;
            if (line == start_line_) end_first_line(position);
            if (end_line_ != std::numeric_limits<std::uintmax_t>::max() && line == end_line_ - 1)
                end_selection(position);
            feed(chunk, base, index, index + 1);
            from = index + 1;
            ++newlines_;
            line_start_ = position + 1;
            if (newlines_ == start_line_) begin(line_start_);
            if (end_line_ != std::numeric_limits<std::uintmax_t>::max() && newlines_ == end_line_ - 1)
                last_line_start_ = line_start_;
        }
        feed(chunk, base, from, chunk.size());
        position_ += chunk.size();
    }

    void begin(std::uintmax_t start) {
        start_ = start;
        if (end_line_ != std::numeric_limits<std::uintmax_t>::max() && start_line_ == end_line_ - 1)
            last_line_start_ = start;
        selection_open_ = true;
        first_line_open_ = true;
    }

    void end_first_line(std::uintmax_t position) {
        first_line_end_ = position;
        if (first_line_open_) {
            first_line_bytes_ += first_line_decoder_.decode({}, true).size();
            first_line_open_ = false;
        }
    }

    void end_selection(std::uintmax_t position) {
        end_ = position;
        if (selection_open_) {
            selected_bytes_ += selection_decoder_.decode({}, true).size();
            selection_open_ = false;
        }
    }

    void feed(std::string_view chunk, std::uintmax_t base, std::size_t from, std::size_t to) {
        // Decoding the whole file drops a leading byte-order mark.
        if (bom_ && base + from < 3) from = std::min(to, static_cast<std::size_t>(3 - base));
        if (to <= from) return;
        const std::string_view bytes = chunk.substr(from, to - from);
        // The frozen scanner counts the decoded sizes as the ranges feed;
        // the final flush only completes a held-back partial character.
        if (selection_open_) selected_bytes_ += selection_decoder_.decode(bytes).size();
        if (first_line_open_) first_line_bytes_ += first_line_decoder_.decode(bytes).size();
    }

    std::uintmax_t start_line_;
    std::uintmax_t end_line_;
    std::uintmax_t position_{};
    std::uintmax_t newlines_{};
    std::uintmax_t line_start_{};
    std::optional<std::uintmax_t> start_{};
    std::optional<std::uintmax_t> end_{};
    std::optional<std::uintmax_t> first_line_end_{};
    std::optional<std::uintmax_t> last_line_start_{};
    std::size_t selected_bytes_{};
    std::size_t first_line_bytes_{};
    bool bom_{};
    bool selection_open_{};
    bool first_line_open_{};
    Utf8Decoder selection_decoder_{};
    Utf8Decoder first_line_decoder_{};
    std::optional<std::string> head_{std::string{}};
};

/// One streaming pass locating lines `[start_line, end_line)` (end absent: to
/// the end of the file) and counting the whole file's newlines, exactly like
/// frozen `BinaryReader.scanLines`. The file is read from position 0 in
/// bounded chunks; nothing unbounded is ever loaded.
support::Expected<LineScan> scan_lines(Environment& env,
        const std::string& path,
        std::uintmax_t start_line,
        std::optional<std::uintmax_t> end_line,
        std::stop_token stop) {
    LineScanner scanner(start_line, end_line.value_or(std::numeric_limits<std::uintmax_t>::max()));
    std::uintmax_t position = 0;
    while (true) {
        if (stop.stop_requested())
            return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted"));
        auto chunk = env.read(path, position, kReadChunkBytes, stop);
        if (!chunk) return std::unexpected(chunk.error());
        if (chunk->empty()) break;
        position += chunk->size();
        scanner.push(*chunk);
    }
    return scanner.finish();
}

/// The decoded start of bytes `[start, end)`: all of it, or enough for
/// `truncate_head_of` (more than `kReadToolMaxBytes + 1` decoded bytes or
/// `kReadToolMaxLines` newlines), exactly like frozen `readHead`.
support::Expected<std::string> read_head(Environment& env,
        const std::string& path,
        std::uintmax_t start,
        std::uintmax_t end,
        bool skip_bom,
        std::stop_token stop) {
    Utf8Decoder decoder;
    std::string text;
    std::size_t newlines = 0;
    for (std::uintmax_t position = skip_bom && start == 0 ? 3 : start; position < end;) {
        if (stop.stop_requested())
            return std::unexpected(support::make_error(support::ErrorCode::Cancelled, "aborted"));
        const auto want = static_cast<std::size_t>(std::min<std::uintmax_t>(kReadChunkBytes, end - position));
        auto chunk = env.read(path, position, want, stop);
        if (!chunk) return std::unexpected(chunk.error());
        if (chunk->empty()) break;
        position += chunk->size();
        const std::string decoded = decoder.decode(*chunk);
        text += decoded;
        newlines += static_cast<std::size_t>(std::ranges::count(decoded, '\n'));
        if (newlines >= kReadToolMaxLines || text.size() > kReadToolMaxBytes + 1) return text;
    }
    return text + decoder.decode({}, true);
}

struct Truncation {
    std::string content{};
    bool truncated{};
    std::string_view truncated_by{}; // "lines", "bytes", empty when not truncated
    double total_lines{};
    double total_bytes{};
    std::size_t output_lines{};
    std::size_t output_bytes{};
    bool first_line_exceeds_limit{};
};

/// Frozen `truncateHeadOf` of a text known by a prefix and its totals. The
/// prefix must be the whole text, or longer than `kReadToolMaxBytes + 1`
/// bytes, or hold at least `kReadToolMaxLines` newlines; then the result
/// equals truncating the whole text.
Truncation truncate_head_of(std::string_view prefix, double total_lines, double total_bytes) {
    std::vector<std::string_view> lines;
    if (!prefix.empty()) {
        for (std::size_t start = 0; start <= prefix.size();) {
            const std::size_t end = prefix.find('\n', start);
            if (end == std::string_view::npos) {
                lines.push_back(prefix.substr(start));
                break;
            }
            lines.push_back(prefix.substr(start, end - start));
            start = end + 1;
        }
        if (prefix.ends_with('\n')) lines.pop_back();
    }
    Truncation result{.content = std::string(prefix),
            .truncated = false,
            .truncated_by = {},
            .total_lines = total_lines,
            .total_bytes = total_bytes,
            .output_lines = static_cast<std::size_t>(total_lines),
            .output_bytes = static_cast<std::size_t>(total_bytes),
            .first_line_exceeds_limit = false};
    if (total_lines <= kReadToolMaxLines && total_bytes <= kReadToolMaxBytes) return result;
    if (!lines.empty() && lines.front().size() > kReadToolMaxBytes) {
        result.content.clear();
        result.truncated = true;
        result.truncated_by = "bytes";
        result.output_lines = 0;
        result.output_bytes = 0;
        result.first_line_exceeds_limit = true;
        return result;
    }
    std::vector<std::string_view> kept;
    std::size_t output_bytes = 0;
    std::string_view truncated_by = "lines";
    for (std::size_t i = 0; i < lines.size() && i < kReadToolMaxLines; ++i) {
        const std::size_t line_bytes = lines[i].size() + (i > 0 ? 1 : 0); // +1 for the newline
        if (output_bytes + line_bytes > kReadToolMaxBytes) {
            truncated_by = "bytes";
            break;
        }
        kept.push_back(lines[i]);
        output_bytes += line_bytes;
    }
    if (truncated_by != "bytes") truncated_by = kept.size() < total_lines ? "lines" : "bytes";
    result.content.clear();
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i > 0) result.content.push_back('\n');
        result.content.append(kept[i]);
    }
    result.truncated = true;
    result.truncated_by = truncated_by;
    result.output_lines = kept.size();
    result.output_bytes = result.content.size();
    return result;
}

struct ReadToolDiagnostic {
    std::string severity{};
    std::string code{};
    std::string message{};
};

struct ReadTextResult {
    std::string text{};
    support::JsonValue::object_t details{};
    bool has_details{};
    std::vector<ReadToolDiagnostic> diagnostics{};
};

/// The frozen `readText`: one-based line `offset`/`limit` select like
/// `Array.prototype.slice`, an offset at or past the newline-past line fails,
/// and the selected range is truncated at the default bounds with the frozen
/// diagnostics and `details.truncation` shape.
support::Expected<ReadTextResult> read_text(Environment& env,
        const std::string& path,
        const std::optional<double> offset,
        const std::optional<double> limit,
        std::stop_token stop) {
    const double start_line = offset.has_value() && *offset != 0 ? std::max(0.0, *offset - 1) : 0;
    const double start_line_display = start_line + 1;
    const double slice_start = slice_index(start_line);
    const double scan_start_double = is_safe_integer(slice_start) ? slice_start : 0;
    const std::optional<double> requested_end =
            limit.has_value() ? std::optional(std::max(scan_start_double + 1, slice_index(start_line + *limit)))
                              : std::nullopt;
    const std::optional<std::uintmax_t> scan_end = requested_end.has_value() && is_safe_integer(*requested_end)
                                                           ? std::optional(static_cast<std::uintmax_t>(*requested_end))
                                                           : std::nullopt;
    const std::uintmax_t scan_start = static_cast<std::uintmax_t>(scan_start_double);

    auto scan = scan_lines(env, path, scan_start, scan_end, stop);
    if (!scan) return std::unexpected(scan.error());
    const double total_file_lines = static_cast<double>(scan->newlines) + 1;
    if (start_line >= total_file_lines) {
        return std::unexpected(support::make_error(support::ErrorCode::Tool,
                std::format("Offset {} is beyond end of file ({} lines total)",
                        js_number(*offset),
                        js_number(total_file_lines))));
    }

    double user_limited_lines = 0;
    double selected_line_count = total_file_lines - slice_start;
    if (limit.has_value()) {
        const double end_line = std::min(start_line + *limit, total_file_lines);
        user_limited_lines = end_line - start_line;
        const double relative_end = slice_index(end_line);
        const double slice_end = relative_end < 0 ? std::max(total_file_lines + relative_end, 0.0) : relative_end;
        selected_line_count = std::max(0.0, slice_end - slice_start);
        if (selected_line_count > 0 && relative_end < 0) {
            // A negative end counts from the end of the file; only the line
            // count tells where it lands, so the selection is scanned again.
            scan = scan_lines(env, path, scan_start, static_cast<std::uintmax_t>(slice_end), stop);
            if (!scan) return std::unexpected(scan.error());
        }
    }
    const bool empty = selected_line_count == 0;
    const bool ends_with_newline = !empty && scan->last_line_start == scan->end && scan->last_line_start > scan->start;
    const double total_lines =
            (empty || scan->selected_bytes == 0) ? 0 : selected_line_count - (ends_with_newline ? 1 : 0);
    const double total_bytes = empty ? 0 : static_cast<double>(scan->selected_bytes);

    std::string head;
    if (!empty) {
        auto first_bytes = env.read(path, 0, 3, stop);
        if (!first_bytes) return std::unexpected(first_bytes.error());
        const bool bom = first_bytes->size() == 3 && static_cast<unsigned char>((*first_bytes)[0]) == 0xef &&
                         static_cast<unsigned char>((*first_bytes)[1]) == 0xbb &&
                         static_cast<unsigned char>((*first_bytes)[2]) == 0xbf;
        auto read = read_head(env, path, scan->start, scan->end, bom, stop);
        if (!read) return std::unexpected(read.error());
        head = std::move(*read);
    }

    const Truncation truncation = truncate_head_of(head, total_lines, total_bytes);
    ReadTextResult result{.text = truncation.content};
    const auto truncation_json = [&truncation]() {
        support::JsonValue::object_t value;
        value.emplace("truncated", support::JsonValue(truncation.truncated));
        value.emplace("truncatedBy", support::JsonValue(std::string(truncation.truncated_by)));
        value.emplace("totalLines", support::JsonValue(truncation.total_lines));
        value.emplace("totalBytes", support::JsonValue(truncation.total_bytes));
        value.emplace("outputLines", support::JsonValue(static_cast<double>(truncation.output_lines)));
        value.emplace("outputBytes", support::JsonValue(static_cast<double>(truncation.output_bytes)));
        value.emplace("lastLinePartial", support::JsonValue(false));
        value.emplace("firstLineExceedsLimit", support::JsonValue(truncation.first_line_exceeds_limit));
        value.emplace("maxLines", support::JsonValue(static_cast<double>(kReadToolMaxLines)));
        value.emplace("maxBytes", support::JsonValue(static_cast<double>(kReadToolMaxBytes)));
        return value;
    };
    if (truncation.first_line_exceeds_limit) {
        // Show the start of the line, cut at the byte limit on a character
        // boundary (frozen `characterEnd`).
        const bool integral = std::floor(start_line) == start_line;
        std::string_view line =
                integral && !head.empty() ? std::string_view(head).substr(0, head.find('\n')) : std::string_view{};
        std::size_t end = std::min(line.size(), kReadToolMaxBytes);
        while (end > 0 && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80)
            --end;
        const std::size_t line_size = integral ? scan->first_line_bytes : 0;
        result.text = std::string(line.substr(0, end));
        result.diagnostics.push_back(ReadToolDiagnostic{.severity = "warn",
                .code = "truncated",
                .message = std::format("Line {} is {}, exceeds the {} limit; showing its first {}. "
                                       "Use bash: sed -n '{}p' {} | tail -c +{}",
                        js_number(start_line_display),
                        format_size(line_size),
                        format_size(kReadToolMaxBytes),
                        format_size(end),
                        js_number(start_line_display),
                        path,
                        end + 1)});
        auto details = truncation_json();
        details["outputLines"] = support::JsonValue(1.0);
        details["outputBytes"] = support::JsonValue(static_cast<double>(end));
        result.details.emplace("truncation", support::JsonValue(std::move(details)));
        result.has_details = true;
    } else if (truncation.truncated) {
        const double end_line_display = start_line_display + static_cast<double>(truncation.output_lines) - 1;
        const double next_offset = end_line_display + 1;
        const std::string limit_text = truncation.truncated_by == "lines"
                                               ? std::string{}
                                               : std::format(" ({} limit)", format_size(kReadToolMaxBytes));
        result.diagnostics.push_back(ReadToolDiagnostic{.severity = "info",
                .code = "truncated",
                .message = std::format("Showing lines {}-{} of {}{}. Use offset={} to continue.",
                        js_number(start_line_display),
                        js_number(end_line_display),
                        js_number(total_file_lines),
                        limit_text,
                        js_number(next_offset))});
        result.details.emplace("truncation", support::JsonValue(truncation_json()));
        result.has_details = true;
    } else if (limit.has_value() && start_line + user_limited_lines < total_file_lines) {
        const double remaining = total_file_lines - (start_line + user_limited_lines);
        const double next_offset = start_line + user_limited_lines + 1;
        result.diagnostics.push_back(ReadToolDiagnostic{.severity = "info",
                .code = {},
                .message = std::format("{} more lines in file. Use offset={} to continue.",
                        js_number(remaining),
                        js_number(next_offset))});
    }
    return result;
}

Result run(std::string_view name,
        const support::JsonValue& args,
        Environment& env,
        const CodingToolOptions& options,
        std::stop_token stop) {
    if (name == "bash") {
        const auto* command = string_field(args, "command");
        if (!command) return failure("Invalid command");
        std::chrono::milliseconds timeout{0};
        if (auto* value = field(args, "timeout"); value && value->holds<double>() && value->get_number() > 0)
            timeout = std::chrono::milliseconds{static_cast<std::int64_t>(value->get_number() * 1000)};
        auto result = env.exec(*command, timeout, {}, stop);
        if (!result) return std::unexpected(result.error());
        if (result->exit_code != 0)
            return failure(result->output + std::format("\nCommand exited with code {}", result->exit_code));
        return text_result(std::move(result->output));
    }
    const auto* path = string_field(args, "path");
    if (!path) return failure("Invalid path");
    if (name == "write") {
        const auto* content = string_field(args, "content");
        if (!content) return failure("Invalid content");
        // Frozen pi routes `write` through the same per-file mutation queue as
        // `edit`, so a write and an edit of one file cannot interleave.
        const auto guard = acquire_file_mutation(mutation_key(env, *path, stop));
        auto result = env.write(*path, *content, stop);
        if (!result) return std::unexpected(result.error());
        return text_result("Successfully wrote to " + *path);
    }
    return read_path(*path, env, options, stop);
}

/// A failed text read keeps the frozen harness error shape its caller sees;
/// cancellation propagates so an aborted task settles aborted.
ai::ToolResultMessage read_failure(const support::Error& error) {
    if (error.code == support::ErrorCode::Cancelled)
        return ai::ToolResultMessage{.content = harness_error_content(error.message), .is_error = true};
    const std::string& message = error.message;
    const bool wrapped = message.starts_with("<harness>");
    return ai::ToolResultMessage{
            .content = wrapped ? std::vector<ai::Content>{ai::text_content(message)} : harness_error_content(message),
            .is_error = true};
}

/// The durable `read` tool over the context hook: text reads honor the frozen
/// one-based line offset/limit with the bounded default truncation, and the
/// truncation record rides the result's `details` (frozen `ReadToolDetails`).
support::Expected<ai::ToolResultMessage> run_read_durable(
        const support::JsonValue& args, Environment& env, const CodingToolOptions& options, std::stop_token stop) {
    const auto* path = string_field(args, "path");
    if (!path) return ai::ToolResultMessage{.content = harness_error_content("Invalid path"), .is_error = true};
    const std::optional<double> offset = field(args, "offset") != nullptr && field(args, "offset")->holds<double>()
                                                 ? std::optional(field(args, "offset")->get_number())
                                                 : std::nullopt;
    const std::optional<double> limit = field(args, "limit") != nullptr && field(args, "limit")->holds<double>()
                                                ? std::optional(field(args, "limit")->get_number())
                                                : std::nullopt;
    if ((offset.has_value() && !std::isfinite(*offset)) || (limit.has_value() && !std::isfinite(*limit)))
        return ai::ToolResultMessage{.content = harness_error_content("Invalid offset/limit"), .is_error = true};

    // The image path reads the whole file, exactly like frozen `readImage`.
    auto head_bytes = env.read(*path, 0, 4096, stop);
    if (!head_bytes) return read_failure(head_bytes.error());
    if (const auto mime_type = detect_image_mime_type(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(head_bytes->data()), head_bytes->size()));
            mime_type.has_value()) {
        auto content = env.read(*path, stop);
        if (!content) return read_failure(content.error());
        const std::span<const std::uint8_t> bytes{
                reinterpret_cast<const std::uint8_t*>(content->data()), content->size()};
        const ReadImage image{.path = *path, .bytes = bytes, .mime_type = *mime_type};
        auto prepared =
                options.images ? prepare_with_processor(image, options) : prepare_without_processor(image, options);
        if (!prepared)
            return ai::ToolResultMessage{
                    .content = {ai::text_content(std::string(prepared.error().message))}, .is_error = true};
        return ai::ToolResultMessage{.content = image_result(std::move(*prepared), options.vision)};
    }

    auto text = read_text(env, *path, offset, limit, stop);
    if (!text) return read_failure(text.error());
    ai::ToolResultMessage message{};
    if (!text->text.empty()) message.content.push_back(ai::text_content(std::move(text->text)));
    if (!text->diagnostics.empty()) {
        // Frozen `appendToolResult` ends the stored content with the rendered
        // diagnostics, so the message is exactly what the model sees.
        std::string rendered = "<harness>\n";
        for (std::size_t i = 0; i < text->diagnostics.size(); ++i) {
            if (i > 0) rendered.push_back('\n');
            rendered += std::format("[{}] {}", text->diagnostics[i].severity, text->diagnostics[i].message);
        }
        rendered += "\n</harness>";
        message.content.push_back(ai::text_content(std::move(rendered)));
    }
    if (text->has_details) message.details = support::JsonValue(std::move(text->details));
    return message;
}

/// A rejected edit keeps the frozen `<harness>` diagnostic and carries no
/// details: the model sees the reason and the run continues.
ai::ToolResultMessage edit_failure(std::string message) {
    return ai::ToolResultMessage{.content = harness_error_content(message), .is_error = true};
}

/// One `edit` call: the frozen BOM/CRLF repair, the per-file mutation slot, and
/// the diff payload the model-visible result carries.
ai::ToolResultMessage run_edit(const support::JsonValue& args, Environment& env, std::stop_token stop) {
    const auto* path = string_field(args, "path");
    if (!path) return edit_failure("Invalid path");
    const auto* edits = field(args, "edits");
    if (!edits || !edits->holds<support::JsonValue::array_t>() || edits->get_array().empty())
        return edit_failure("Edit tool input is invalid. edits must contain at least one replacement.");
    std::vector<EditReplacement> replacements;
    replacements.reserve(edits->get_array().size());
    for (const auto& edit : edits->get_array()) {
        const auto* old_text = string_field(edit, "oldText");
        const auto* new_text = string_field(edit, "newText");
        if (!old_text || !new_text) return edit_failure("Invalid edit");
        replacements.push_back(EditReplacement{.old_text = *old_text, .new_text = *new_text});
    }

    std::optional<AppliedEdits> applied;
    {
        // The slot covers the whole read-modify-write, so a concurrent call of
        // one file reads what this call wrote instead of the stale text.
        const auto guard = acquire_file_mutation(mutation_key(env, *path, stop));
        auto content = env.read(*path, stop);
        if (!content) return edit_failure(content.error().message);
        const auto stripped = strip_bom(*content);
        const auto original_ending = detect_line_ending(stripped.text);
        auto current = apply_edits_to_normalized_content(normalize_to_lf(stripped.text), replacements, *path);
        if (!current) return edit_failure(current.error().message);
        auto written =
                env.write(*path, stripped.bom + restore_line_endings(current->new_content, original_ending), stop);
        if (!written) return edit_failure(written.error().message);
        applied = std::move(*current);
    }

    const auto display = generate_diff_string(applied->base_content, applied->new_content);
    support::JsonValue::object_t details;
    details.emplace("diff", support::JsonValue(display.diff));
    details.emplace(
            "patch", support::JsonValue(generate_unified_patch(*path, applied->base_content, applied->new_content)));
    if (display.first_changed_line)
        details.emplace("firstChangedLine", support::JsonValue(*display.first_changed_line));
    return ai::ToolResultMessage{
            .content = {ai::text_content(
                    std::format("Successfully replaced {} block(s) in {}.", replacements.size(), *path))},
            .details = support::JsonValue(std::move(details)),
    };
}
support::JsonValue parameters(std::string_view name) {
    using Json = support::JsonValue;
    auto string_schema = [] { return Json::object_t{{"type", "string"}}; };
    auto number_schema = [] { return Json::object_t{{"type", "number"}}; };
    Json::object_t properties;
    Json::array_t required;
    auto add_string = [&](std::string key) {
        properties[key] = string_schema();
        required.emplace_back(key);
    };
    if (name == "bash") {
        add_string("command");
        properties["timeout"] = number_schema();
    } else {
        add_string("path");
        if (name == "write") add_string("content");
        if (name == "read") {
            // The frozen read tool's one-based line offset/limit (issue #1197).
            properties["offset"] = number_schema();
            properties["limit"] = number_schema();
        }
        if (name == "edit") {
            Json::object_t edit_properties{{"oldText", string_schema()}, {"newText", string_schema()}};
            Json::object_t item{{"type", "object"},
                    {"properties", std::move(edit_properties)},
                    {"required", Json::array_t{"oldText", "newText"}}};
            properties["edits"] = Json::object_t{{"type", "array"}, {"items", std::move(item)}};
            required.emplace_back("edits");
        }
    }
    return Json::object_t{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}
} // namespace

Extension coding_tools(std::shared_ptr<Environment> environment, CodingToolOptions options) {
    Extension result{.name = "coding-tools"};
    // The four tool lambdas are copied into the Registry and share one
    // immutable options object for as long as their registrations live.
    auto shared_options = std::make_shared<const CodingToolOptions>(std::move(options));
    for (std::string name : {"read", "write", "edit", "bash"}) {
        ToolRegistration registration{
                .declaration = ai::Tool{
                        .name = name, .description = "Durable " + name + " tool", .parameters = parameters(name)}};
        if (name == "edit") {
            // The edit result carries structured `details`, which only the
            // context-aware hook can return.
            registration.execute_durable = [environment](support::JsonValue args, ToolExecutionContext context) {
                if (!environment)
                    return support::Expected<ai::ToolResultMessage>{edit_failure("No execution environment")};
                const auto stop = context.runtime != nullptr ? context.runtime->stop_token() : std::stop_token{};
                return support::Expected<ai::ToolResultMessage>{run_edit(args, *environment, stop)};
            };
        } else if (name == "read") {
            // The read tool reports structured truncation `details` with its
            // rendered diagnostics (frozen `ReadToolDetails`), which only the
            // context-aware hook can return.
            registration.execute_durable = [environment, shared_options](
                                                   support::JsonValue args, ToolExecutionContext context) {
                if (!environment)
                    return support::Expected<ai::ToolResultMessage>{ai::ToolResultMessage{
                            .content = harness_error_content("No execution environment"), .is_error = true}};
                const auto stop = context.runtime != nullptr ? context.runtime->stop_token() : std::stop_token{};
                return run_read_durable(args, *environment, *shared_options, stop);
            };
        } else {
            registration.execute = [environment, name, shared_options](support::JsonValue args, std::stop_token stop) {
                if (!environment)
                    return support::AsyncResult<std::vector<ai::Content>>{failure("No execution environment")};
                return support::AsyncResult<std::vector<ai::Content>>{
                        run(name, args, *environment, *shared_options, stop)};
            };
        }
        result.tools.push_back(std::move(registration));
    }
    return result;
}
} // namespace cch::durable
