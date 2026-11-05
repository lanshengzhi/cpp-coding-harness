#include "coding_agent/ImageInput.hpp"
#include "coding_agent/tui/ImageTranscoder.hpp"

#include "support/ImageCapabilitiesGuard.hpp"
#include "support/ImageFixture.hpp"
#include "support/PiTuiEvidence.hpp"

#include <cch/tui/Image.hpp>
#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace cch;

namespace {

using tests::ImageCapabilitiesGuard;

cch::tui::Image make_deferred_image(
        std::string encoded_data, std::string mime_type, cch::tui::ImageCellConstraints constraints = {}) {
    return cch::tui::Image(
            cch::tui::ImageContent{
                    .encoded_data = std::move(encoded_data),
                    .mime_type = std::move(mime_type),
            },
            cch::tui::ImageOptions{
                    .constraints = constraints,
                    .defer_protocol_selection = true,
            });
}

std::string joined_screen(const cch::tui::VirtualTerminal& terminal) {
    std::string text;
    for (const auto& line : terminal.screen()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

} // namespace

TEST_CASE("ensure_png_transcoder registers the product converter for Kitty terminals only",
        "[coding_agent][tui][image][transcoder][issue996][spec]") {
    std::size_t registrations = 0;
    const auto notify = [&registrations] { ++registrations; };
    cch::tui::clear_image_transcoder();

    {
        const ImageCapabilitiesGuard iterm2({.images = cch::tui::InlineImageProtocol::ITerm2});
        coding_agent::tui::ensure_png_transcoder(notify);
        CHECK(registrations == 0);
        CHECK_FALSE(cch::tui::to_png(std::string{tests::kTinyJpegBase64}, "image/jpeg"));
    }
    {
        const ImageCapabilitiesGuard none({.images = cch::tui::InlineImageProtocol::None});
        coding_agent::tui::ensure_png_transcoder(notify);
        CHECK(registrations == 0);
        CHECK_FALSE(cch::tui::to_png(std::string{tests::kTinyJpegBase64}, "image/jpeg"));
    }

    {
        const ImageCapabilitiesGuard kitty({.images = cch::tui::InlineImageProtocol::Kitty});
        coding_agent::tui::ensure_png_transcoder(notify);
        CHECK(registrations == 1);
        const auto converted = cch::tui::to_png(std::string{tests::kTinyJpegBase64}, "image/jpeg");
        REQUIRE(converted);
        CHECK(cch::tui::get_png_dimensions(*converted) == (cch::tui::ImagePixelSize{.width = 2, .height = 2}));

        // Registration happens once: a later call neither reloads nor notifies.
        coding_agent::tui::ensure_png_transcoder(notify);
        CHECK(registrations == 1);
    }

    // The host can cancel the capability, and a replacement registration drops
    // the previous cache instead of serving conversions it produced.
    cch::tui::clear_image_transcoder();
    CHECK_FALSE(cch::tui::to_png(std::string{tests::kTinyJpegBase64}, "image/jpeg"));
    std::size_t replacements = 0;
    cch::tui::set_image_transcoder([&replacements](std::string_view, std::string_view) {
        ++replacements;
        return std::optional<std::string>{std::string{"converted"}};
    });
    CHECK(cch::tui::to_png(std::string{tests::kTinyJpegBase64}, "image/jpeg") ==
            std::optional<std::string>{std::string{"converted"}});
    CHECK(replacements == 1);
    cch::tui::clear_image_transcoder();
}

TEST_CASE("The product transcoder re-encodes non-PNG input as oriented PNG",
        "[coding_agent][tui][image][transcoder][issue996][spec]") {
    // The fixture carries EXIF orientation 6, so its JPEG header is 3x2 while
    // the decoded image is 2x3: the converted PNG must report the oriented
    // size, not the on-the-wire header size.
    REQUIRE(cch::tui::get_jpeg_dimensions(tests::kOrientedJpegBase64) ==
            (cch::tui::ImagePixelSize{.width = 3, .height = 2}));
    const auto oriented = coding_agent::transcode_image_to_png(tests::kOrientedJpegBase64, "image/jpeg");
    REQUIRE(oriented);
    CHECK(cch::tui::get_png_dimensions(*oriented) == (cch::tui::ImagePixelSize{.width = 2, .height = 3}));

    const auto tiny_png = coding_agent::transcode_image_to_png(tests::kTinyPngBase64, "image/png");
    REQUIRE(tiny_png);
    CHECK(cch::tui::get_png_dimensions(*tiny_png) == (cch::tui::ImagePixelSize{.width = 2, .height = 2}));

    const auto tiny_gif = coding_agent::transcode_image_to_png(tests::kTinyGifBase64, "image/gif");
    REQUIRE(tiny_gif);
    CHECK(cch::tui::get_png_dimensions(*tiny_gif) == (cch::tui::ImagePixelSize{.width = 1, .height = 1}));

    CHECK_FALSE(coding_agent::transcode_image_to_png("not base64 \x1b_G raw", "image/jpeg"));
}

TEST_CASE("Kitty placement of a non-PNG image is sized from the converted PNG",
        "[coding_agent][tui][image][transcoder][issue996][spec]") {
    const ImageCapabilitiesGuard kitty({.images = cch::tui::InlineImageProtocol::Kitty});
    cch::tui::clear_image_transcoder();
    cch::tui::VirtualTerminal terminal({
            .columns = 30,
            .rows = 8,
            .capabilities =
                    {
                            .synchronized_output = true,
                            .inline_images = cch::tui::InlineImageProtocol::Kitty,
                            .cell_pixels = cch::tui::CellPixelDimensions{.width = 10, .height = 10},
                    },
    });
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Image>(make_deferred_image(
            std::string{tests::kOrientedJpegBase64}, "image/jpeg", {.max_width = 3, .max_height = 3}))));
    REQUIRE(tui.start());

    // Without a registered converter the image keeps its text fallback labelled
    // with the encoded payload's own dimensions, and no Kitty placement is
    // emitted for a JPEG payload.
    REQUIRE(tui.render());
    CHECK(terminal.images().empty());
    CHECK(joined_screen(terminal).find("[Image: [image/jpeg] 3x2]") != std::string::npos);

    cch::tui::set_image_transcoder(coding_agent::tui::load_png_transcoder());
    REQUIRE(tui.render());
    REQUIRE(terminal.images().size() == 1);
    const auto& image = terminal.images()[0];
    CHECK(image.mime_type == "image/png");
    CHECK(image.pixel_width == 2);
    CHECK(image.pixel_height == 3);
    // Cell sizing follows the oriented PNG (2x3 -> 2 columns by 3 rows), not the
    // encoded JPEG header (3x2 -> 3 columns by 2 rows).
    CHECK(image.region.columns == 2);
    CHECK(image.region.rows == 3);

    cch::tui::clear_image_transcoder();
}

// #1201: the recaptured frozen pi-v1.3.0 observation of the image transcoder
// primitive (registered `audit-1201/image-transcoder.json`) replays through
// the C++ conversion seam. The frozen capture observed the Kitty text
// fallback without a transcoder, conversion through a working transcoder, a
// fresh image retrying after a failing transcoder, and the fallback returning
// once the transcoder is cleared; the C++ registry follows the same
// set/convert/retry/clear lifecycle (pi parity: a same-epoch failure stays
// cached, and installing a replacement clears the cache).
TEST_CASE("frozen audit-1201 transcoder fallback convert retry and clear replay in C++",
        "[coding_agent][tui][image][transcoder][issue1201]") {
    const auto artifact = cch::tests::read_pi_v130_audit_evidence("audit-1201", "image-transcoder.json");
    REQUIRE(artifact);
    const std::string fake_jpeg = "QUJDREVGRw==";
    const std::string fake_png =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";

    const auto* scenario = [&]() -> const cch::support::JsonValue* {
        for (const auto& item : artifact->at("scenarios").get_array()) {
            if (item.at("name").get_string() == "without a transcoder a non-PNG image renders the text fallback")
                return &item;
        }
        return nullptr;
    }();
    REQUIRE(scenario != nullptr);
    const auto& fallback_lines = scenario->at("expected").at("lines").get_array();
    REQUIRE(!fallback_lines.empty());

    cch::tui::clear_image_transcoder();

    // 1. Without a transcoder the text fallback renders and no conversion runs.
    {
        cch::tui::clear_image_transcoder();
        CHECK_FALSE(cch::tui::to_png(fake_jpeg, "image/jpeg"));
        CHECK(cch::tui::image_fallback("image/jpeg", cch::tui::ImagePixelSize{.width = 800, .height = 600}) ==
                fallback_lines.at(0).get_string());
    }

    // 2. A working transcoder converts the non-PNG payload.
    {
        std::size_t calls = 0;
        cch::tui::set_image_transcoder([&](std::string_view, std::string_view) {
            ++calls;
            return std::optional<std::string>{fake_png};
        });
        CHECK(cch::tui::to_png(fake_jpeg, "image/jpeg") == std::optional<std::string>{fake_png});
        CHECK(calls == 1);
    }

    // 3. A failing transcoder keeps the fallback; a replacement clears the
    //    cache so the same payload converts under the new transcoder.
    {
        std::size_t bad_calls = 0;
        cch::tui::set_image_transcoder([&](std::string_view, std::string_view) {
            ++bad_calls;
            return std::optional<std::string>{};
        });
        CHECK_FALSE(cch::tui::to_png(fake_jpeg, "image/jpeg"));

        std::size_t good_calls = 0;
        cch::tui::set_image_transcoder([&](std::string_view, std::string_view) {
            ++good_calls;
            return std::optional<std::string>{fake_png};
        });
        CHECK(cch::tui::to_png(fake_jpeg, "image/jpeg") == std::optional<std::string>{fake_png});
        CHECK(bad_calls == 1);
        CHECK(good_calls == 1);
    }

    // 4. Clearing the transcoder restores the fallback.
    {
        cch::tui::clear_image_transcoder();
        CHECK_FALSE(cch::tui::to_png(fake_jpeg, "image/jpeg"));
    }
    cch::tui::clear_image_transcoder();
}
