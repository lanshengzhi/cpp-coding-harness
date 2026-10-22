#include <cch/tui/TerminalImage.hpp>

#include <cch/tui/Base64.hpp>

#include <cch/support/Error.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "support/UniqueFd.hpp"

// Behavioral baseline: pi 83114817 packages/tui/src/terminal-image.ts
// (detectCapabilities env rules, probeTmuxHyperlinks, hyperlink,
// shortenImagePath/imageFallback with pathToFileURL linking, encodeKitty with
// 4096-byte chunks, deleteKittyImage, encodeITerm2 with default
// preserveAspectRatio omitted, and the CSI 16 t cell-size response consumed by
// tui.ts consumeCellSizeResponse).

namespace cch::tui {
namespace {

constexpr std::size_t kKittyChunkSize = 4096;
constexpr std::size_t kCellSizePendingMax = 64;
constexpr std::string_view kCellSizeResponsePrefix{"\x1b[6;"};

[[nodiscard]] std::string lowercase_environment(std::string_view name) {
    const auto* value = std::getenv(std::string(name).c_str());
    std::string result = value == nullptr ? "" : value;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

/// JS-truthy env presence: a set-but-empty variable is absent, exactly like
/// `process.env.NAME` in pi's detectCapabilities.
[[nodiscard]] bool environment_present(std::string_view name) {
    const auto* value = std::getenv(std::string(name).c_str());
    return value != nullptr && *value != '\0';
}

[[nodiscard]] std::string home_directory() {
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return home;
    }
    if (const auto* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
        return profile;
    }
    return {};
}

/// Node `path.isAbsolute` semantics: a leading slash or backslash, or a
/// drive-letter prefix (pi links only absolute paths).
[[nodiscard]] bool is_absolute_path(std::string_view path) {
    if (path.starts_with('/') || path.starts_with('\\')) return true;
    return path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

/// Node `pathToFileURL` percent-encoding: unreserved characters plus the
/// path-safe set (`: ; = , @ & + $ ! * ( ) ' - _ . /`) stay literal, every
/// other byte is percent-encoded as UTF-8 (spaces, `?`, `#`, `%`, `~`, `[`,
/// `]`, quotes, angle brackets, and non-ASCII bytes).
[[nodiscard]] std::string file_url_encode(std::string_view path) {
    std::string result;
    result.reserve(path.size());
    for (const auto byte : path) {
        const auto character = static_cast<unsigned char>(byte);
        const auto literal = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            byte == '-' || byte == '_' || byte == '.' ||
            byte == '!' || byte == '*' || byte == '(' || byte == ')' ||
            byte == '\'' || byte == ':' || byte == ';' || byte == '=' ||
            byte == ',' || byte == '@' || byte == '&' || byte == '+' ||
            byte == '$' || byte == '/';
        if (literal) {
            result.push_back(byte);
            continue;
        }
        result += std::format("%{:02X}", character);
    }
    return result;
}

[[nodiscard]] std::string path_to_file_url(std::string_view absolute_path) {
    return "file://" + file_url_encode(absolute_path);
}

/// Shorten home-prefixed absolute paths to `~/...` for compact display,
/// exactly as pi's `shortenImagePath`.
[[nodiscard]] std::string shorten_image_path(std::string_view filename) {
    const auto home = home_directory();
    if (home.empty() || filename.size() < home.size()) return std::string(filename);
    const auto home_prefix = std::string_view(home);
    if (filename == home_prefix) return "~";
    if (filename.starts_with(home_prefix) && filename.size() > home_prefix.size() &&
        (filename[home_prefix.size()] == '/' || filename[home_prefix.size()] == '\\')) {
        return "~" + std::string(filename.substr(home_prefix.size()));
    }
    return std::string(filename);
}

[[nodiscard]] bool intervals_intersect(
    std::size_t left_start,
    std::size_t left_size,
    std::size_t right_start,
    std::size_t right_size) {
    if (left_size == 0 || right_size == 0) return false;
    if (left_start <= right_start) return right_start - left_start < left_size;
    return left_start - right_start < right_size;
}


/// Frozen `cropKittyImageLine` geometry for this image: the source pixel band
/// the placement still shows, or an unset rectangle when frozen would emit no
/// crop controls.
[[nodiscard]] detail::KittyImageCrop image_crop(const TerminalImage& image) {
    const auto total_rows = image.source_rows == 0 ? image.region.rows : image.source_rows;
    return detail::kitty_image_crop(image.pixel_height, total_rows, image.source_row_offset, image.region.rows);
}

/// Kitty transmit-and-place controls. A placement cut by an ancestor scrolled
/// or clipped region drops `r=` and appends the frozen crop controls in
/// `y=,h=,r=` order (pi `cropKittyImageLine`).
[[nodiscard]] std::string kitty_transmission_parameters(const TerminalImage& image, TerminalImageHandle handle) {
    const auto crop = image_crop(image);
    if (crop.rows == 0) {
        return std::format("a=T,f=100,q=2,C=1,c={},r={},i={}", image.region.columns, image.region.rows, handle.value);
    }
    return std::format("a=T,f=100,q=2,C=1,c={},i={},y={},h={},r={}",
            image.region.columns,
            handle.value,
            crop.source_y,
            crop.source_height,
            crop.rows);
}

/// Placement-only controls for a resource already uploaded in this session
/// (pi `getKittyImagePlacement`): `a=p` instead of the transmission, with the
/// same placement controls and the identical crop.
[[nodiscard]] std::string kitty_placement_parameters(const TerminalImage& image, TerminalImageHandle handle) {
    const auto crop = image_crop(image);
    if (crop.rows == 0) {
        return std::format("a=p,q=2,C=1,c={},r={},i={}", image.region.columns, image.region.rows, handle.value);
    }
    return std::format("a=p,q=2,C=1,c={},i={},y={},h={},r={}",
            image.region.columns,
            handle.value,
            crop.source_y,
            crop.source_height,
            crop.rows);
}

[[nodiscard]] std::string encode_kitty(const TerminalImage& image, TerminalImageHandle handle) {
    auto parameters = kitty_transmission_parameters(image, handle);
    if (image.encoded_data.size() <= kKittyChunkSize) {
        return std::format("\x1b_G{};{}\x1b\\", parameters, image.encoded_data);
    }

    std::string result;
    for (std::size_t offset = 0; offset < image.encoded_data.size(); offset += kKittyChunkSize) {
        const auto size = std::min(kKittyChunkSize, image.encoded_data.size() - offset);
        const auto last = offset + size == image.encoded_data.size();
        if (offset == 0) {
            result += std::format(
                "\x1b_G{},m=1;{}\x1b\\",
                parameters,
                image.encoded_data.substr(offset, size));
        } else {
            result += std::format(
                "\x1b_Gm={};{}\x1b\\",
                last ? 0 : 1,
                image.encoded_data.substr(offset, size));
        }
    }
    return result;
}

[[nodiscard]] std::string encode_iterm2(const TerminalImage& image) {
    // Pi's encodeITerm2 omits preserveAspectRatio when it keeps the default
    // (true); only `preserveAspectRatio=0` is emitted.
    auto decoded_size = image.encoded_data.size() / 4 * 3;
    if (image.encoded_data.ends_with("=="))
        decoded_size -= 2;
    else if (image.encoded_data.ends_with('='))
        --decoded_size;
    auto parameters =
            std::format("inline=1;width={};height={};size={}", image.region.columns, image.region.rows, decoded_size);
    if (image.filename) parameters += ";name=" + base64_encode(*image.filename);
    return std::format("\x1b]1337;File={}:{}\x07", parameters, image.encoded_data);
}

[[nodiscard]] bool cell_size_fragment_character(char value) {
    return (value >= '0' && value <= '9') || value == ';';
}

/// Trim ASCII whitespace from both ends (the tmux feature list is
/// comma-separated with possibly padded entries).
[[nodiscard]] std::string_view trim_ascii_space(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

/// Whether the comma-separated tmux `client_termfeatures` list contains the
/// `hyperlinks` feature (pi's probeTmuxHyperlinks parsing).
[[nodiscard]] bool features_include_hyperlinks(std::string_view features) {
    std::size_t start = 0;
    while (start <= features.size()) {
        const auto separator = features.find(',', start);
        const auto end = separator == std::string_view::npos ? features.size() : separator;
        if (trim_ascii_space(features.substr(start, end - start)) == "hyperlinks") {
            return true;
        }
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    return false;
}

/// The input starts with the full `ESC [ 6 ;` prefix and everything after it
/// could still become a response (digits and semicolons only).
[[nodiscard]] bool is_cell_size_fragment(std::string_view input) {
    if (!input.starts_with(kCellSizeResponsePrefix)) return false;
    for (const auto& byte : input.substr(kCellSizeResponsePrefix.size())) {
        if (!cell_size_fragment_character(byte)) return false;
    }
    return true;
}

[[nodiscard]] std::optional<detail::CellSizeResponse> parse_cell_size_response(
    std::string_view response) {
    if (!response.starts_with(kCellSizeResponsePrefix) || response.size() <= kCellSizeResponsePrefix.size() ||
        response.back() != 't') {
        return std::nullopt;
    }
    const auto body = response.substr(
        kCellSizeResponsePrefix.size(),
        response.size() - kCellSizeResponsePrefix.size() - 1);
    const auto separator = body.find(';');
    if (separator == std::string_view::npos) return std::nullopt;
    const auto height = body.substr(0, separator);
    const auto width = body.substr(separator + 1);

    std::size_t height_px = 0;
    std::size_t width_px = 0;
    const auto [height_end, height_error] =
        std::from_chars(height.data(), height.data() + height.size(), height_px);
    const auto [width_end, width_error] =
        std::from_chars(width.data(), width.data() + width.size(), width_px);
    if (height_error != std::errc{} || width_error != std::errc{} ||
        height_end != height.data() + height.size() ||
        width_end != width.data() + width.size()) {
        return std::nullopt;
    }
    return detail::CellSizeResponse{
        .height_px = height_px,
        .width_px = width_px,
    };
}

} // namespace

bool detail::probe_tmux_hyperlinks() {
    constexpr auto kProbeTimeout = std::chrono::milliseconds(250);
    std::array<int, 2> descriptors{};
    if (::pipe(descriptors.data()) != 0) return false;
    cch::support::UniqueFd read_end(descriptors[0]);
    cch::support::UniqueFd write_end(descriptors[1]);
    const auto child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        (void)::dup2(write_end.get(), STDOUT_FILENO);
        ::execlp(
            "tmux",
            "tmux",
            "display-message",
            "-p",
            "#{client_termfeatures}",
            static_cast<char*>(nullptr));
        ::_exit(127);
    }
    (void)write_end.close();

    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + kProbeTimeout;
    std::array<char, 256> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd item{.fd = read_end.get(), .events = POLLIN, .revents = 0};
        const auto ready = ::poll(&item, 1, static_cast<int>(remaining.count()));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0 || (item.revents & POLLIN) == 0) break;
        const auto count = ::read(read_end.get(), buffer.data(), buffer.size());
        if (count <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    (void)read_end.close();
    (void)::kill(child, SIGKILL);
    int status = 0;
    (void)::waitpid(child, &status, 0);
    return features_include_hyperlinks(output);
}

DetectedImageCapabilities detect_image_capabilities(TmuxHyperlinkProbe tmux_forwards_hyperlink) {
    const auto term_program = lowercase_environment("TERM_PROGRAM");
    const auto terminal_emulator = lowercase_environment("TERMINAL_EMULATOR");
    const auto term = lowercase_environment("TERM");
    const auto color_term = lowercase_environment("COLORTERM");
    const auto true_color_hint = color_term == "truecolor" || color_term == "24bit" || term.ends_with("-direct");
    const auto pi_true_color = lowercase_environment("PI_TRUE_COLOR");

    auto detected = [&]() -> DetectedImageCapabilities {
        const auto color = true_color_hint ? TerminalColorCapability::TrueColor : TerminalColorCapability::Xterm256;
        if (environment_present("TMUX") || term.starts_with("tmux")) {
            return {.images = InlineImageProtocol::None,
                    .hyperlinks = tmux_forwards_hyperlink ? tmux_forwards_hyperlink() : false,
                    .color = color};
        }
        if (term.starts_with("screen")) {
            return {.images = InlineImageProtocol::None, .hyperlinks = false, .color = color};
        }
        // Herdr forwards OSC 8 hyperlinks and supports the Kitty graphics
        // protocol. Check it before outer-terminal detection because variables
        // such as KITTY_WINDOW_ID leak into its panes; true colour stays the
        // COLORTERM/TERM hint rather than a forced value.
        if (term_program == "herdr") {
            return {.images = InlineImageProtocol::Kitty, .hyperlinks = true, .color = color};
        }
        if (environment_present("KITTY_WINDOW_ID") || term_program == "kitty") {
            return {.images = InlineImageProtocol::Kitty,
                    .hyperlinks = true,
                    .color = TerminalColorCapability::TrueColor};
        }
        if (term_program == "ghostty" || term.find("ghostty") != std::string::npos ||
                environment_present("GHOSTTY_RESOURCES_DIR")) {
            return {.images = InlineImageProtocol::Kitty,
                    .hyperlinks = true,
                    .color = TerminalColorCapability::TrueColor};
        }
        if (environment_present("WEZTERM_PANE") || term_program == "wezterm") {
            return {.images = InlineImageProtocol::Kitty,
                    .hyperlinks = true,
                    .color = TerminalColorCapability::TrueColor};
        }
        if (term_program == "warpterminal" || environment_present("WARP_SESSION_ID") ||
                environment_present("WARP_TERMINAL_SESSION_UUID")) {
            return {.images = InlineImageProtocol::Kitty,
                    .hyperlinks = true,
                    .color = TerminalColorCapability::TrueColor};
        }
        if (environment_present("ITERM_SESSION_ID") || term_program == "iterm.app") {
            return {.images = InlineImageProtocol::ITerm2,
                    .hyperlinks = true,
                    .color = TerminalColorCapability::TrueColor};
        }
        if (environment_present("WT_SESSION") || term_program == "vscode" || term_program == "alacritty" ||
                term_program == "zed") {
            return {.images = InlineImageProtocol::None,
                    .hyperlinks = true,
                    .color = TerminalColorCapability::TrueColor};
        }
        if (terminal_emulator == "jetbrains-jediterm") {
            return {.images = InlineImageProtocol::None,
                    .hyperlinks = false,
                    .color = TerminalColorCapability::TrueColor};
        }
        return {.images = InlineImageProtocol::None, .hyperlinks = false, .color = color};
    }();

    const auto pi_hyperlinks = lowercase_environment("PI_HYPERLINKS");
    if (pi_hyperlinks == "1")
        detected.hyperlinks = true;
    else if (pi_hyperlinks == "0")
        detected.hyperlinks = false;

    const auto pi_image_protocol = lowercase_environment("PI_IMAGE_PROTOCOL");
    if (pi_image_protocol == "kitty")
        detected.images = InlineImageProtocol::Kitty;
    else if (pi_image_protocol == "iterm2")
        detected.images = InlineImageProtocol::ITerm2;
    else if (pi_image_protocol == "none" || pi_image_protocol == "0")
        detected.images = InlineImageProtocol::None;

    if (pi_true_color == "1")
        detected.color = TerminalColorCapability::TrueColor;
    else if (pi_true_color == "0")
        detected.color = TerminalColorCapability::Xterm256;
    return detected;
}

std::string hyperlink(std::string_view text, std::string_view url) {
    return std::format("\x1b]8;;{}\x1b\\{}\x1b]8;;\x1b\\", url, text);
}

std::string image_fallback(
    std::string_view mime_type,
    std::optional<ImagePixelSize> dimensions,
    std::optional<std::string_view> filename) {
    std::string result = "[Image: ";
    if (filename) {
        const auto display = shorten_image_path(*filename);
        if (get_image_capabilities().hyperlinks && is_absolute_path(*filename)) {
            result += hyperlink(display, path_to_file_url(*filename));
        } else {
            result += display;
        }
        result += " ";
    }
    result += std::format("[{}]", mime_type);
    if (dimensions) {
        result += std::format(" {}x{}", dimensions->width, dimensions->height);
    }
    result += "]";
    return result;
}

namespace detail {

KittyImageCrop kitty_image_crop(
        std::size_t pixel_height, std::size_t total_rows, std::size_t hidden_rows, std::size_t visible_rows) {
    if (total_rows == 0 || hidden_rows >= total_rows || visible_rows == 0) return {};
    const auto cropped_rows = std::min(visible_rows, total_rows - hidden_rows);
    if (hidden_rows == 0 && cropped_rows == total_rows) return {};
    const auto source_y = pixel_height * hidden_rows / total_rows;
    const auto source_end = (pixel_height * (hidden_rows + cropped_rows) + total_rows - 1) / total_rows;
    return {
            .source_y = source_y,
            .source_height = std::max<std::size_t>(1, std::min(pixel_height, source_end) - source_y),
            .rows = cropped_rows,
    };
}

std::string encode_kitty_image_placement(const TerminalImage& image, TerminalImageHandle handle) {
    return std::format("\x1b_G{}\x1b\\", kitty_placement_parameters(image, handle));
}

CellSizeInputResult consume_cell_size_input(
    std::string pending,
    std::string_view input) {
    CellSizeInputResult result{
        .pending = std::move(pending),
        .forwarded_input = {},
        .responses = {},
    };
    result.pending += input;
    while (!result.pending.empty()) {
        if (!result.pending.starts_with("\x1b")) {
            const auto escape = result.pending.find('\x1b');
            const auto count = escape == std::string::npos ? result.pending.size() : escape;
            result.forwarded_input += result.pending.substr(0, count);
            result.pending.erase(0, count);
            continue;
        }
        if (!result.pending.starts_with(kCellSizeResponsePrefix)) {
            // Not even a partial response prefix (a bare ESC, ESC [, ESC [6):
            // forward it and keep scanning.
            result.forwarded_input += result.pending.substr(0, 1);
            result.pending.erase(0, 1);
            continue;
        }
        const auto final = result.pending.find('t', kCellSizeResponsePrefix.size());
        if (final == std::string::npos) {
            if (result.pending.size() > kCellSizePendingMax) {
                result.forwarded_input += result.pending;
                result.pending.clear();
                continue;
            }
            if (is_cell_size_fragment(result.pending)) return result;
            // Contains an invalid character before any 't': not a response.
            result.forwarded_input += result.pending;
            result.pending.clear();
            continue;
        }
        const auto response_text = result.pending.substr(0, final + 1);
        auto response = parse_cell_size_response(response_text);
        if (!response) {
            // The first 't' did not close a valid response: forward the prefix
            // fragment (so scanning can continue past it) and retry.
            result.forwarded_input += result.pending.substr(0, 1);
            result.pending.erase(0, 1);
            continue;
        }
        result.responses.push_back(*response);
        result.pending.erase(0, final + 1);
    }
    return result;
}

bool cell_regions_intersect(
    const CellRegion& left,
    const CellRegion& right) {
    return intervals_intersect(left.column, left.columns, right.column, right.columns) &&
        intervals_intersect(left.row, left.rows, right.row, right.rows);
}

bool protocol_supports_mime(
    InlineImageProtocol protocol,
    std::string_view mime_type) {
    if (protocol == InlineImageProtocol::Kitty) return mime_type == "image/png";
    if (protocol == InlineImageProtocol::ITerm2) {
        return mime_type == "image/png" || mime_type == "image/jpeg" ||
            mime_type == "image/gif" || mime_type == "image/webp";
    }
    return false;
}

support::Expected<std::string> encode_terminal_image(
    InlineImageProtocol protocol,
    const TerminalImage& image,
    TerminalImageHandle handle) {
    if (image.region.columns == 0 || image.region.rows == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Inline image region must be positive"));
    }
    if (!protocol_supports_mime(protocol, image.mime_type)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Inline image format is unsupported by the terminal protocol"));
    }
    if (protocol == InlineImageProtocol::Kitty) {
        detail::register_image_metadata(handle,
                {
                        .resource_id = image.resource_id,
                        .revision = image.revision,
                        .filename = image.filename ? std::optional<std::string>(*image.filename) : std::nullopt,
                        .mime_type = std::string(image.mime_type),
                        .pixel_width = image.pixel_width,
                        .pixel_height = image.pixel_height,
                });
        return encode_kitty(image, handle);
    }
    if (protocol == InlineImageProtocol::ITerm2) return encode_iterm2(image);
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "Terminal does not support inline images"));
}

support::Expected<std::string> encode_terminal_image_removal(
    InlineImageProtocol protocol,
    TerminalImageHandle handle) {
    if (protocol == InlineImageProtocol::Kitty) {
        return delete_kitty_image(handle);
    }
    if (protocol == InlineImageProtocol::ITerm2) return std::string{};
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "Terminal does not support inline images"));
}

support::Expected<std::string> encode_terminal_image_removal_all(InlineImageProtocol protocol) {
    if (protocol == InlineImageProtocol::Kitty) return delete_all_kitty_images();
    if (protocol == InlineImageProtocol::ITerm2) return std::string{};
    return std::unexpected(
            support::make_error(support::ErrorCode::Validation, "Terminal does not support inline images"));
}

} // namespace detail

} // namespace cch::tui
