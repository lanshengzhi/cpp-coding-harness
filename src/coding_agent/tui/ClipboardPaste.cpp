#include "ClipboardPaste.hpp"

#include "coding_agent/ImageInput.hpp"
#include "coding_agent/tui/ClipboardReader.hpp"
#include "support/UniqueFd.hpp"

#include <cch/support/Error.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] std::string clipboard_uuid() {
    std::random_device random;
    std::array<std::uint8_t, 16> bytes{};
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::string value;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) value.push_back('-');
        value += std::format("{:02x}", bytes[index]);
    }
    return value;
}

[[nodiscard]] support::Expected<std::filesystem::path> write_clipboard_image(
    std::span<const std::uint8_t> bytes,
    std::string_view extension) {
    std::error_code temp_error;
    const auto temp_directory = std::filesystem::temp_directory_path(temp_error);
    if (temp_error || temp_directory.empty()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "clipboard temporary directory is unavailable",
            temp_error.message()));
    }

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        std::filesystem::path path;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            path = temp_directory /
                std::format("pi-clipboard-{}{}", clipboard_uuid(), extension);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& error) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not generate a clipboard image path",
                error.what()));
        } catch (...) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not generate a clipboard image path"));
        }
#endif
        support::UniqueFd fd(::open(
            path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0600));
        if (!fd) {
            if (errno == EEXIST) continue;
            return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not create clipboard image file",
                std::error_code(errno, std::generic_category()).message()));
        }

        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(
                fd.get(),
                bytes.data() + written,
                bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const auto write_error = errno;
                (void)fd.close();
                std::error_code remove_error;
                std::filesystem::remove(path, remove_error);
                return std::unexpected(support::make_error(
                    support::ErrorCode::Process,
                    "could not write clipboard image file",
                    std::error_code(write_error, std::generic_category()).message()));
            }
            written += static_cast<std::size_t>(count);
        }
        if (fd.close() != 0) {
            const auto close_error = errno;
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
            return std::unexpected(support::make_error(
                support::ErrorCode::Process,
                "could not finish clipboard image file",
                std::error_code(close_error, std::generic_category()).message()));
        }
        return path;
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::Process,
        "could not allocate a unique clipboard image path"));
}

} // namespace

boost::asio::awaitable<std::optional<std::string>> read_clipboard_insert_content(
    AsyncClipboardReader& reader) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        auto image = co_await reader.read_image();
        if (image && *image && !(*image)->bytes.empty()) {
            const auto mime_type = sniff_supported_image_mime_type((*image)->bytes);
            const auto extension = mime_type
                ? extension_for_image_mime_type(*mime_type)
                : std::nullopt;
            if (extension) {
                const auto path = write_clipboard_image((*image)->bytes, *extension);
                if (path) {
                    co_return path->string();
                }
            }
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        const auto ignored = support::make_error(
            support::ErrorCode::Unknown,
            "clipboard image read failed",
            error.what());
        (void)ignored;
    } catch (...) {
        const auto ignored = support::make_error(
            support::ErrorCode::Unknown,
            "clipboard image read failed");
        (void)ignored;
    }
#endif

#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        auto text = co_await reader.read_text();
        if (text && *text && !(*text)->empty()) {
            co_return std::move(**text);
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        const auto ignored = support::make_error(
            support::ErrorCode::Unknown,
            "clipboard text read failed",
            error.what());
        (void)ignored;
    } catch (...) {
        const auto ignored = support::make_error(
            support::ErrorCode::Unknown,
            "clipboard text read failed");
        (void)ignored;
    }
#endif
    co_return std::nullopt;
}

} // namespace cch::coding_agent::tui
