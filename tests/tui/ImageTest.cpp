#include <cch/tui/Container.hpp>
#include <cch/tui/Image.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "tui/TerminalImage.hpp"
#include "tui/UnicodeWidth.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const auto remaining = bytes.size() - offset;
        const auto first = static_cast<std::uint32_t>(bytes[offset]);
        const auto second = remaining > 1 ? static_cast<std::uint32_t>(bytes[offset + 1]) : 0U;
        const auto third = remaining > 2 ? static_cast<std::uint32_t>(bytes[offset + 2]) : 0U;
        const auto value = (first << 16U) | (second << 8U) | third;
        result.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
        result.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
        result.push_back(remaining > 1 ? kAlphabet[(value >> 6U) & 0x3fU] : '=');
        result.push_back(remaining > 2 ? kAlphabet[value & 0x3fU] : '=');
    }
    return result;
}

std::vector<std::uint8_t> truncated_png_header(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> bytes(24, 0);
    const std::uint8_t signature[]{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    for (std::size_t index = 0; index < 8; ++index) bytes[index] = signature[index];
    bytes[12] = 'I';
    bytes[13] = 'H';
    bytes[14] = 'D';
    bytes[15] = 'R';
    bytes[16] = static_cast<std::uint8_t>(width >> 24U);
    bytes[17] = static_cast<std::uint8_t>(width >> 16U);
    bytes[18] = static_cast<std::uint8_t>(width >> 8U);
    bytes[19] = static_cast<std::uint8_t>(width);
    bytes[20] = static_cast<std::uint8_t>(height >> 24U);
    bytes[21] = static_cast<std::uint8_t>(height >> 16U);
    bytes[22] = static_cast<std::uint8_t>(height >> 8U);
    bytes[23] = static_cast<std::uint8_t>(height);
    return bytes;
}

std::vector<std::uint8_t> png_header(std::uint32_t width, std::uint32_t height) {
    auto bytes = truncated_png_header(width, height);
    bytes.resize(45, 0);
    bytes[8] = 0;
    bytes[9] = 0;
    bytes[10] = 0;
    bytes[11] = 13;
    bytes[24] = 8;
    bytes[25] = 6;
    bytes[37] = 'I';
    bytes[38] = 'E';
    bytes[39] = 'N';
    bytes[40] = 'D';
    return bytes;
}

std::vector<std::uint8_t> jpeg_header(std::uint16_t width, std::uint16_t height) {
    return {
        0xff, 0xd8,
        0xff, 0xc0, 0x00, 0x11, 0x08,
        static_cast<std::uint8_t>(height >> 8U), static_cast<std::uint8_t>(height),
        static_cast<std::uint8_t>(width >> 8U), static_cast<std::uint8_t>(width),
        0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00,
        0xff, 0xd9,
    };
}

std::vector<std::uint8_t> gif_header(std::uint16_t width, std::uint16_t height) {
    return {
        'G', 'I', 'F', '8', '9', 'a',
        static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(width >> 8U),
        static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(height >> 8U),
        0, 0, 0, 0x3b,
    };
}

std::vector<std::uint8_t> webp_vp8x_header(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> bytes(30, 0);
    bytes[0] = 'R'; bytes[1] = 'I'; bytes[2] = 'F'; bytes[3] = 'F';
    bytes[8] = 'W'; bytes[9] = 'E'; bytes[10] = 'B'; bytes[11] = 'P';
    bytes[4] = 22;
    bytes[12] = 'V'; bytes[13] = 'P'; bytes[14] = '8'; bytes[15] = 'X';
    bytes[16] = 10;
    --width;
    --height;
    bytes[24] = static_cast<std::uint8_t>(width);
    bytes[25] = static_cast<std::uint8_t>(width >> 8U);
    bytes[26] = static_cast<std::uint8_t>(width >> 16U);
    bytes[27] = static_cast<std::uint8_t>(height);
    bytes[28] = static_cast<std::uint8_t>(height >> 8U);
    bytes[29] = static_cast<std::uint8_t>(height >> 16U);
    return bytes;
}

cch::tui::Image make_image(
    std::string encoded_data,
    std::string mime_type,
    std::optional<std::string> filename = std::nullopt,
    cch::tui::ImageCellConstraints constraints = {}) {
    return cch::tui::Image(
        cch::tui::ImageContent{
            .encoded_data = std::move(encoded_data),
            .mime_type = std::move(mime_type),
            .filename = std::move(filename),
        },
        cch::tui::ImageOptions{.constraints = constraints});
}

} // namespace

TEST_CASE("Image reports private PNG JPEG GIF and WebP dimensions", "[tui][image][issue53]") {
    struct Fixture {
        std::string mime_type;
        std::vector<std::uint8_t> bytes;
        std::size_t width;
        std::size_t height;
    };
    const std::vector<Fixture> fixtures{
        {.mime_type = "image/png", .bytes = png_header(320, 200), .width = 320, .height = 200},
        {.mime_type = "image/jpeg", .bytes = jpeg_header(640, 480), .width = 640, .height = 480},
        {.mime_type = "image/gif", .bytes = gif_header(40, 30), .width = 40, .height = 30},
        {.mime_type = "image/webp", .bytes = webp_vp8x_header(1024, 512), .width = 1024, .height = 512},
    };

    for (const auto& fixture : fixtures) {
        auto image = make_image(base64_encode(fixture.bytes), fixture.mime_type, "fixture.img");
        const auto rendered = image.render(80);
        REQUIRE(rendered);
        REQUIRE(rendered->images.size() == 1);
        CHECK(rendered->images[0].pixel_width == fixture.width);
        CHECK(rendered->images[0].pixel_height == fixture.height);
        CHECK(rendered->lines[0].find(std::to_string(fixture.width) + "x" +
                                      std::to_string(fixture.height)) != std::string::npos);
    }
}

TEST_CASE("Truncated supported formats use fallback instead of native transport", "[tui][image][issue53]") {
    auto jpeg = jpeg_header(640, 480);
    jpeg.resize(jpeg.size() - 2);
    auto gif = gif_header(40, 30);
    gif.pop_back();
    auto webp_bad_riff_size = webp_vp8x_header(1024, 512);
    --webp_bad_riff_size[4];
    auto webp_bad_chunk_size = webp_vp8x_header(1024, 512);
    ++webp_bad_chunk_size[16];

    struct Fixture {
        std::string mime_type;
        std::vector<std::uint8_t> bytes;
    };
    const std::vector<Fixture> fixtures{
        {.mime_type = "image/png", .bytes = truncated_png_header(320, 200)},
        {.mime_type = "image/jpeg", .bytes = std::move(jpeg)},
        {.mime_type = "image/gif", .bytes = std::move(gif)},
        {.mime_type = "image/webp", .bytes = std::move(webp_bad_riff_size)},
        {.mime_type = "image/webp", .bytes = std::move(webp_bad_chunk_size)},
    };

    for (const auto& fixture : fixtures) {
        auto image = make_image(base64_encode(fixture.bytes), fixture.mime_type, "truncated.img");
        const auto rendered = image.render(80);

        REQUIRE(rendered);
        CHECK(rendered->images.empty());
        REQUIRE(rendered->lines.size() == 1);
        CHECK(rendered->lines[0].find("unavailable") != std::string::npos);
    }
}

TEST_CASE("Unsupported and malformed images render bounded themed fallback", "[tui][image][issue53]") {
    cch::tui::ImageOptions options;
    options.fallback_style = [](std::string text) { return "\x1b[33m" + text + "\x1b[0m"; };
    cch::tui::Image image(
        cch::tui::ImageContent{
            .encoded_data = "not base64 \x1b_G raw",
            .mime_type = "image/tiff\nunsafe",
            .filename = "very-long\x1b[2J-name.tiff",
        },
        std::move(options));

    const auto rendered = image.render(18);

    REQUIRE(rendered);
    CHECK(rendered->images.empty());
    REQUIRE(rendered->lines.size() == 1);
    CHECK(cch::tui::detail::visible_width(rendered->lines[0]) <= 18);
    CHECK(rendered->lines[0].find("not base64") == std::string::npos);
    CHECK(rendered->lines[0].find("\x1b_G") == std::string::npos);
}

TEST_CASE("Terminal without image capability shows semantic fallback", "[tui][image][issue53]") {
    cch::tui::VirtualTerminal terminal({.columns = 40, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(16, 9)),
        "image/png",
        "fallback.png"))));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    CHECK(terminal.images().empty());
    CHECK(terminal.screen()[0].find("Image") != std::string::npos);
    CHECK(terminal.screen()[0].find("16x9") != std::string::npos);
}

TEST_CASE("Kitty and iTerm2 images stay inside TUI-owned rows", "[tui][image][issue53]") {
    const auto data = base64_encode(png_header(20, 20));
    for (const auto protocol : {cch::tui::InlineImageProtocol::Kitty, cch::tui::InlineImageProtocol::ITerm2}) {
        cch::tui::VirtualTerminal terminal({
            .columns = 6,
            .rows = 3,
            .capabilities = {
                .synchronized_output = true,
                .inline_images = protocol,
                .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
            },
        });
        cch::tui::Tui tui(terminal);
        REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("top", 0, 0)));
        REQUIRE(tui.add_child(std::make_unique<cch::tui::Image>(make_image(
            data,
            "image/png",
            "test.png",
            {.max_width = 2, .max_height = 2}))));
        REQUIRE(tui.start());
        REQUIRE(tui.render());

        REQUIRE(terminal.images().size() == 1);
        const cch::tui::CellRegion expected_region{
            .column = 0, .row = 1, .columns = 2, .rows = 2};
        CHECK(terminal.images()[0].region == expected_region);
        CHECK(terminal.images()[0].region.row + terminal.images()[0].region.rows <= 3);
    }
}

TEST_CASE("Image replacement removal resize and stop clear stale regions", "[tui][image][issue53]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 8,
        .rows = 4,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    cch::tui::Tui tui(terminal);
    auto image = std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(20, 20)),
        "image/png",
        "first.png",
        {.max_width = 2, .max_height = 2}));
    auto* image_ptr = image.get();
    REQUIRE(tui.add_child(std::move(image)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(terminal.images().size() == 1);
    const auto first_handle = terminal.images()[0].handle;

    image_ptr->set_content({
        .encoded_data = base64_encode(png_header(10, 30)),
        .mime_type = "image/png",
        .filename = "second.png",
    });
    REQUIRE(tui.render());
    REQUIRE(terminal.images().size() == 1);
    CHECK(terminal.images()[0].handle != first_handle);
    CHECK(terminal.images()[0].filename == std::optional<std::string>{"second.png"});

    REQUIRE(terminal.inject_resize({.columns = 8, .rows = 1}));
    REQUIRE(tui.render());
    CHECK(terminal.images().empty());

    image_ptr->clear();
    REQUIRE(tui.render());
    CHECK(terminal.images().empty());
    REQUIRE(tui.stop());
    CHECK(terminal.images().empty());
}

TEST_CASE("Kitty uses PNG only while iTerm2 accepts validated named formats", "[tui][image][issue53]") {
    const auto jpeg = base64_encode(jpeg_header(20, 20));
    cch::tui::VirtualTerminal kitty({
        .columns = 20,
        .rows = 3,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    cch::tui::Tui kitty_tui(kitty);
    REQUIRE(kitty_tui.add_child(std::make_unique<cch::tui::Image>(make_image(jpeg, "image/jpeg"))));
    REQUIRE(kitty_tui.start());
    REQUIRE(kitty_tui.render());
    CHECK(kitty.images().empty());
    CHECK(kitty.screen()[0].find("Image") != std::string::npos);

    struct ITermFixture {
        std::string mime_type;
        std::string encoded_data;
    };
    const std::vector<ITermFixture> fixtures{
        {.mime_type = "image/png", .encoded_data = base64_encode(png_header(20, 20))},
        {.mime_type = "image/jpeg", .encoded_data = jpeg},
        {.mime_type = "image/gif", .encoded_data = base64_encode(gif_header(20, 20))},
        {.mime_type = "image/webp", .encoded_data = base64_encode(webp_vp8x_header(20, 20))},
    };
    for (const auto& fixture : fixtures) {
        cch::tui::VirtualTerminal iterm({
            .columns = 20,
            .rows = 3,
            .capabilities = {
                .synchronized_output = true,
                .inline_images = cch::tui::InlineImageProtocol::ITerm2,
                .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
            },
        });
        cch::tui::Tui iterm_tui(iterm);
        REQUIRE(iterm_tui.add_child(std::make_unique<cch::tui::Image>(make_image(
            fixture.encoded_data,
            fixture.mime_type,
            std::nullopt,
            {.max_width = 2, .max_height = 2}))));
        REQUIRE(iterm_tui.start());
        REQUIRE(iterm_tui.render());
        REQUIRE(iterm.images().size() == 1);
        CHECK(iterm.images()[0].mime_type == fixture.mime_type);
    }
}

TEST_CASE("Overlay image materialization respects the overlay width allocation", "[tui][image][overlay][issue53]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 30,
        .rows = 3,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    cch::tui::Tui tui(terminal);
    cch::tui::OverlayOptions options;
    options.size_constraints.max_width = 10;
    auto overlay = std::make_unique<cch::tui::Overlay>(options);
    REQUIRE(overlay->add_child(std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(100, 10)),
        "image/png"))));
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.images().size() == 1);
    CHECK(terminal.images()[0].region.columns <= 10);
}

TEST_CASE("Narrow overlays preserve lower images outside their allocation", "[tui][image][overlay][issue53]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 30,
        .rows = 2,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    cch::tui::Tui tui(terminal);
    auto box = std::make_unique<cch::tui::Box>(10, 0);
    REQUIRE(box->add_child(std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(10, 10)),
        "image/png",
        "lower.png",
        {.max_width = 1, .max_height = 1}))));
    REQUIRE(tui.add_child(std::move(box)));

    cch::tui::OverlayOptions options;
    options.size_constraints.max_width = 4;
    auto overlay = std::make_unique<cch::tui::Overlay>(options);
    REQUIRE(overlay->add_child(std::make_unique<cch::tui::Text>("menu")));
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.images().size() == 1);
    CHECK(terminal.images()[0].region.column == 10);
    CHECK(terminal.images()[0].filename == std::optional<std::string>{"lower.png"});
}

TEST_CASE("Container and Box translate nested image regions", "[tui][image][issue53]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 8,
        .rows = 5,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    cch::tui::Tui tui(terminal);
    auto box = std::make_unique<cch::tui::Box>(1, 1);
    REQUIRE(box->add_child(std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(10, 10)),
        "image/png",
        std::nullopt,
        {.max_width = 1, .max_height = 1}))));
    REQUIRE(tui.add_child(std::move(box)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.images().size() == 1);
    CHECK(terminal.images()[0].region.column == 1);
    CHECK(terminal.images()[0].region.row == 1);
}

TEST_CASE("Replacing one image preserves an unaffected image handle", "[tui][image][issue53]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 8,
        .rows = 3,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    cch::tui::Tui tui(terminal);
    auto first = std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(10, 10)),
        "image/png",
        "first.png",
        {.max_width = 1, .max_height = 1}));
    auto* first_ptr = first.get();
    REQUIRE(tui.add_child(std::move(first)));
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(10, 10)),
        "image/png",
        "second.png",
        {.max_width = 1, .max_height = 1}))));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(terminal.images().size() == 2);
    const auto second_handle = terminal.images()[1].handle;

    first_ptr->set_content({
        .encoded_data = base64_encode(png_header(20, 10)),
        .mime_type = "image/png",
        .filename = "replacement.png",
    });
    REQUIRE(tui.render());

    REQUIRE(terminal.images().size() == 2);
    const auto second = std::find_if(terminal.images().begin(), terminal.images().end(), [](const auto& image) {
        return image.filename == std::optional<std::string>{"second.png"};
    });
    REQUIRE(second != terminal.images().end());
    CHECK(second->handle == second_handle);
}

TEST_CASE("VirtualTerminal enforces targeted image region ownership", "[tui][image][terminal][issue53]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 4,
        .rows = 2,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    REQUIRE(terminal.start([](std::string) {}, [](cch::tui::TerminalDimensions) {}));
    const cch::tui::TerminalImage image{
        .encoded_data = base64_encode(png_header(10, 10)),
        .mime_type = "image/png",
        .pixel_width = 10,
        .pixel_height = 10,
        .resource_id = 1,
        .revision = 1,
        .region = {.column = 1, .row = 0, .columns = 1, .rows = 1},
    };
    const auto placed = terminal.place_image(image);
    REQUIRE(placed);

    const auto wrong_region = terminal.remove_image(
        *placed,
        {.column = 0, .row = 0, .columns = 1, .rows = 1});
    REQUIRE_FALSE(wrong_region);
    REQUIRE(terminal.images().size() == 1);
    REQUIRE(terminal.remove_image(*placed, image.region));
    CHECK(terminal.images().empty());
}

TEST_CASE("Hiding and restoring an image overlay reconciles owned regions", "[tui][image][overlay][issue53]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 8,
        .rows = 4,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("base", 0, 0)));
    auto overlay = std::make_unique<cch::tui::Overlay>();
    REQUIRE(overlay->add_child(std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(10, 10)),
        "image/png",
        "overlay.png",
        {.max_width = 1, .max_height = 1}))));
    auto* overlay_ptr = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(terminal.images().size() == 1);

    REQUIRE(tui.hide_overlay(overlay_ptr));
    REQUIRE(tui.render());
    CHECK(terminal.images().empty());
    CHECK(terminal.screen()[0].find("base") != std::string::npos);

    REQUIRE(tui.restore_overlay(overlay_ptr));
    REQUIRE(tui.render());
    REQUIRE(terminal.images().size() == 1);
    CHECK(terminal.images()[0].filename == std::optional<std::string>{"overlay.png"});
}

TEST_CASE("Clipped overlay images show fallback inside the visible allocation", "[tui][image][overlay][issue53]") {
    cch::tui::VirtualTerminal terminal({
        .columns = 30,
        .rows = 3,
        .capabilities = {
            .synchronized_output = true,
            .inline_images = cch::tui::InlineImageProtocol::Kitty,
            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
        },
    });
    cch::tui::Tui tui(terminal);
    cch::tui::OverlayOptions options;
    options.position = cch::tui::OverlayPosition::Absolute;
    options.absolute_row = 2;
    auto overlay = std::make_unique<cch::tui::Overlay>(options);
    REQUIRE(overlay->add_child(std::make_unique<cch::tui::Image>(make_image(
        base64_encode(png_header(20, 20)),
        "image/png",
        "clipped.png",
        {.max_width = 20, .max_height = 2}))));
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    CHECK(terminal.images().empty());
    CHECK(terminal.screen()[2].find("Image") != std::string::npos);
}

TEST_CASE("Private protocol encoders expose meaningful bounded parameters", "[tui][image][protocol][issue53]") {
    const cch::tui::TerminalImage image{
        .encoded_data = "QUFBQQ==",
        .mime_type = "image/png",
        .filename = std::string_view{"name.png"},
        .pixel_width = 1,
        .pixel_height = 1,
        .resource_id = 7,
        .revision = 2,
        .region = {.column = 0, .row = 0, .columns = 2, .rows = 3},
    };

    const auto kitty = cch::tui::detail::encode_terminal_image(
        cch::tui::InlineImageProtocol::Kitty, image, {.value = 42});
    REQUIRE(kitty);
    CHECK(kitty->find("f=100") != std::string::npos);
    CHECK(kitty->find("C=1") != std::string::npos);
    CHECK(kitty->find("c=2") != std::string::npos);
    CHECK(kitty->find("r=3") != std::string::npos);
    CHECK(kitty->find("i=42") != std::string::npos);

    const auto iterm = cch::tui::detail::encode_terminal_image(
        cch::tui::InlineImageProtocol::ITerm2, image, {.value = 42});
    REQUIRE(iterm);
    CHECK(iterm->find("inline=1") != std::string::npos);
    CHECK(iterm->find("width=2") != std::string::npos);
    CHECK(iterm->find("height=3") != std::string::npos);
    CHECK(iterm->find("name=bmFtZS5wbmc=") != std::string::npos);

    const auto removed = cch::tui::detail::encode_terminal_image_removal(
        cch::tui::InlineImageProtocol::Kitty,
        {.value = 42});
    REQUIRE(removed);
    CHECK(removed->find("d=I") != std::string::npos);
    CHECK(removed->find("i=42") != std::string::npos);
    CHECK(removed->find("d=A") == std::string::npos);
}
