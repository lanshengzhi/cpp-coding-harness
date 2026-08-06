#include <cch/tui/TerminalImage.hpp>

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
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "util/UniqueFd.hpp"

// Behavioral baseline: pi 83114817 packages/tui/src/terminal-image.ts
// (detectCapabilities env rules, probeTmuxHyperlinks, hyperlink,
// shortenImagePath/imageFallback with pathToFileURL linking, encodeKitty with
// 4096-byte chunks, deleteKittyImage, encodeITerm2 with default
// preserveAspectRatio omitted, and the CSI 16 t cell-size response consumed by
// tui.ts consumeCellSizeResponse).

namespace cch::tui {
namespace {

std::mutex g_image_capabilities_mutex;
std::optional<DetectedImageCapabilities> g_image_capabilities;

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

[[nodiscard]] std::string base64_encode(std::string_view value) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t offset = 0; offset < value.size(); offset += 3) {
        const auto remaining = value.size() - offset;
        const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset]));
        const auto second = remaining > 1
            ? static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + 1]))
            : 0U;
        const auto third = remaining > 2
            ? static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + 2]))
            : 0U;
        const auto bits = (first << 16U) | (second << 8U) | third;
        result.push_back(kAlphabet[(bits >> 18U) & 0x3fU]);
        result.push_back(kAlphabet[(bits >> 12U) & 0x3fU]);
        result.push_back(remaining > 1 ? kAlphabet[(bits >> 6U) & 0x3fU] : '=');
        result.push_back(remaining > 2 ? kAlphabet[bits & 0x3fU] : '=');
    }
    return result;
}

[[nodiscard]] std::string encode_kitty(
    const TerminalImage& image,
    TerminalImageHandle handle) {
    auto parameters = std::format(
        "a=T,f=100,q=2,C=1,c={},r={},i={}",
        image.region.columns,
        image.region.rows,
        handle.value);
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
    auto parameters = std::format(
        "inline=1;width={};height={}",
        image.region.columns,
        image.region.rows);
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
#if defined(__linux__) || defined(__APPLE__)
    constexpr auto kProbeTimeout = std::chrono::milliseconds(250);
    std::array<int, 2> descriptors{};
    if (::pipe(descriptors.data()) != 0) return false;
    cch::util::UniqueFd read_end(descriptors[0]);
    cch::util::UniqueFd write_end(descriptors[1]);
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
#else
    return false;
#endif
}

DetectedImageCapabilities detect_image_capabilities(
    TmuxHyperlinkProbe tmux_forwards_hyperlink) {
    const auto term_program = lowercase_environment("TERM_PROGRAM");
    const auto terminal_emulator = lowercase_environment("TERMINAL_EMULATOR");
    const auto term = lowercase_environment("TERM");

    // Emit OSC 8 hyperlinks only when tmux confirms it forwards. Image
    // protocols are unreliable under tmux, so images stay disabled.
    if (environment_present("TMUX") || term.starts_with("tmux")) {
        return {
            .images = InlineImageProtocol::None,
            .hyperlinks = tmux_forwards_hyperlink ? tmux_forwards_hyperlink() : false,
        };
    }
    // screen does not forward OSC 8 hyperlinks.
    if (term.starts_with("screen")) {
        return {
            .images = InlineImageProtocol::None,
            .hyperlinks = false,
        };
    }

    if (environment_present("KITTY_WINDOW_ID") || term_program == "kitty") {
        return {.images = InlineImageProtocol::Kitty, .hyperlinks = true};
    }
    if (term_program == "ghostty" || term.find("ghostty") != std::string::npos ||
        environment_present("GHOSTTY_RESOURCES_DIR")) {
        return {.images = InlineImageProtocol::Kitty, .hyperlinks = true};
    }
    if (environment_present("WEZTERM_PANE") || term_program == "wezterm") {
        return {.images = InlineImageProtocol::Kitty, .hyperlinks = true};
    }
    if (term_program == "warpterminal" || environment_present("WARP_SESSION_ID") ||
        environment_present("WARP_TERMINAL_SESSION_UUID")) {
        return {.images = InlineImageProtocol::Kitty, .hyperlinks = true};
    }
    if (environment_present("ITERM_SESSION_ID") || term_program == "iterm.app") {
        return {.images = InlineImageProtocol::ITerm2, .hyperlinks = true};
    }
    if (environment_present("WT_SESSION")) {
        return {.images = InlineImageProtocol::None, .hyperlinks = true};
    }
    if (term_program == "vscode") {
        return {.images = InlineImageProtocol::None, .hyperlinks = true};
    }
    if (term_program == "alacritty") {
        return {.images = InlineImageProtocol::None, .hyperlinks = true};
    }
    if (terminal_emulator == "jetbrains-jediterm") {
        return {.images = InlineImageProtocol::None, .hyperlinks = false};
    }
    // Unknown terminal: be conservative, exactly like pi.
    return {.images = InlineImageProtocol::None, .hyperlinks = false};
}

DetectedImageCapabilities get_image_capabilities() {
    std::lock_guard lock(g_image_capabilities_mutex);
    if (!g_image_capabilities) g_image_capabilities = detect_image_capabilities();
    return *g_image_capabilities;
}

void set_image_capabilities(DetectedImageCapabilities capabilities) {
    std::lock_guard lock(g_image_capabilities_mutex);
    g_image_capabilities = capabilities;
}

void reset_image_capabilities_cache() {
    std::lock_guard lock(g_image_capabilities_mutex);
    g_image_capabilities.reset();
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

util::Expected<std::string> encode_terminal_image(
    InlineImageProtocol protocol,
    const TerminalImage& image,
    TerminalImageHandle handle) {
    if (image.region.columns == 0 || image.region.rows == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Inline image region must be positive"));
    }
    if (!protocol_supports_mime(protocol, image.mime_type)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Inline image format is unsupported by the terminal protocol"));
    }
    if (protocol == InlineImageProtocol::Kitty) return encode_kitty(image, handle);
    if (protocol == InlineImageProtocol::ITerm2) return encode_iterm2(image);
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "Terminal does not support inline images"));
}

util::Expected<std::string> encode_terminal_image_removal(
    InlineImageProtocol protocol,
    TerminalImageHandle handle) {
    if (protocol == InlineImageProtocol::Kitty) {
        return std::format("\x1b_Ga=d,d=I,i={},q=2\x1b\\", handle.value);
    }
    if (protocol == InlineImageProtocol::ITerm2) return std::string{};
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "Terminal does not support inline images"));
}

} // namespace detail

} // namespace cch::tui
