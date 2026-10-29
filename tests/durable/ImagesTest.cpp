#include <cch/durable/Images.hpp>
#include "durable/ImageExpectation.hpp"
#include "support/ImageFixture.hpp"
#include "support/ImagePixels.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace cch::tests::durable {
using namespace cch::durable;
namespace {

constexpr int kPixelTolerance = 24;

[[nodiscard]] std::optional<std::string> expected_mime(const support::JsonValue& value) {
    if (value.holds<std::nullptr_t>()) return std::nullopt;
    return value.get_string();
}

[[nodiscard]] const support::JsonValue& frozen_processor_case(std::string_view name) {
    static const auto scenario =
            image_scenario("the Photon processor orients JPEG and WebP, converts BMP, and reports its metadata");
    return scenario.expected().at(std::string{name});
}

[[nodiscard]] ImageDimensions dims(std::size_t width, std::size_t height) {
    return ImageDimensions{.width = width, .height = height};
}

[[nodiscard]] const support::JsonValue& frozen_sniff_case() {
    static const auto scenario = image_scenario("frozen image sniffing rejects animated PNG and JPEG byte 3 0xf7");
    return scenario.expected();
}

[[nodiscard]] ImageDimensions frozen_dimensions(std::string_view name) {
    const auto& value = frozen_sniff_case().at(std::string{name});
    return dims(static_cast<std::size_t>(value.at("width").get_number()),
            static_cast<std::size_t>(value.at("height").get_number()));
}

[[nodiscard]] std::optional<ImageDimensions> dims_of(std::span<const std::uint8_t> bytes, std::string_view mime_type) {
    const auto dimensions = image_dimensions(bytes, mime_type);
    if (!dimensions) return std::nullopt;
    return *dimensions;
}

[[nodiscard]] std::vector<std::uint8_t> tiny_png() { return decode_base64(kTinyPngBase64); }

} // namespace

TEST_CASE(
        "Image detection accepts exactly the provider inline formats plus BMP", "[durable][images][issue1090][spec]") {
    CHECK(detect_image_mime_type(tiny_png()) == "image/png");
    CHECK(detect_image_mime_type(decode_base64(kTinyJpegBase64)) == "image/jpeg");
    CHECK(detect_image_mime_type(decode_base64(kTinyGifBase64)) == "image/gif");
    CHECK(detect_image_mime_type(decode_base64(kTinyWebpBase64)) == "image/webp");
    CHECK(detect_image_mime_type(tests::tiny_bmp()) == "image/bmp");

    SECTION("an animated PNG is not a static image") {
        CHECK_FALSE(detect_image_mime_type(tests::animated_png()).has_value());
    }
    SECTION("JPEG byte 3 0xf7 is rejected") {
        const std::vector<std::uint8_t> rejected{0xff, 0xd8, 0xff, 0xf7, 0x00};
        CHECK_FALSE(detect_image_mime_type(rejected).has_value());
    }
    SECTION("GIF needs the frozen version suffix") {
        const std::vector<std::uint8_t> unsupported{'G', 'I', 'F', '9', '0', 'a'};
        CHECK_FALSE(detect_image_mime_type(unsupported).has_value());
    }
    SECTION("plain bytes are not an image") {
        const std::string text = "not an image";
        CHECK_FALSE(detect_image_mime_type(
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    }
    SECTION("the frozen sniffing results match the artifact") {
        const auto& expected = frozen_sniff_case();
        CHECK(detect_image_mime_type(tests::animated_png()) == expected_mime(expected.at("animatedPng")));
        CHECK(detect_image_mime_type(std::vector<std::uint8_t>{0xff, 0xd8, 0xff, 0xf7, 0x00}) ==
                expected_mime(expected.at("jpeg")));
        CHECK(detect_image_mime_type(tiny_png()) == expected_mime(expected.at("staticPng")));
        CHECK(detect_image_mime_type(decode_base64(kTinyJpegBase64)) == expected_mime(expected.at("tinyJpeg")));
        CHECK(detect_image_mime_type(decode_base64(kTinyGifBase64)) == expected_mime(expected.at("gif")));
        CHECK(detect_image_mime_type(decode_base64(kTinyWebpBase64)) == expected_mime(expected.at("webp")));
        CHECK(detect_image_mime_type(tests::tiny_bmp()) == expected_mime(expected.at("bmp")));
    }
}

TEST_CASE("Image dimensions come from the frozen headers", "[durable][images][issue1090][spec]") {
    CHECK(dims_of(tiny_png(), "image/png") == frozen_dimensions("staticPngDimensions"));
    CHECK(dims_of(decode_base64(kTinyJpegBase64), "image/jpeg") == frozen_dimensions("tinyJpegDimensions"));
    CHECK(dims_of(decode_base64(kTinyGifBase64), "image/gif") == frozen_dimensions("gifDimensions"));
    CHECK(dims_of(decode_base64(kTinyWebpBase64), "image/webp") == frozen_dimensions("webpDimensions"));
    CHECK(dims_of(decode_base64(kOrientedJpegBase64), "image/jpeg") == dims(3, 2));
    CHECK(dims_of(decode_base64(kPatternedJpegBase64), "image/jpeg") == dims(16, 8));

    SECTION("BMP has no header-only dimensions in the frozen reader") {
        CHECK(frozen_sniff_case().at("bmpDimensions").holds<std::nullptr_t>());
        CHECK_FALSE(dims_of(tests::tiny_bmp(), "image/bmp").has_value());
    }
    SECTION("a truncated header cannot be read") {
        auto png = tiny_png();
        png.resize(20);
        CHECK_FALSE(dims_of(png, "image/png").has_value());
    }
}

TEST_CASE("The local image processor passes through an upright inline image within the limits",
        "[durable][images][issue1090][spec]") {
    const auto png = tiny_png();
    const auto prepared = local_image_processor()->prepare(png, "image/png", ImageLimits{});
    REQUIRE(prepared.has_value());
    CHECK(prepared->mime_type == "image/png");
    CHECK(prepared->data == kTinyPngBase64);
    CHECK_FALSE(prepared->resized.has_value());
    CHECK_FALSE(prepared->converted_from.has_value());

    SECTION("a byte limit exactly at the base64 length still passes through") {
        auto limits = ImageLimits{};
        limits.max_base64_bytes = kTinyPngBase64.size();
        CHECK(local_image_processor()->prepare(png, "image/png", limits).has_value());
    }
    SECTION("a byte limit below the native re-encode shrinks to the floor") {
        auto limits = ImageLimits{};
        limits.max_base64_bytes = 100;
        const auto squeezed = local_image_processor()->prepare(png, "image/png", limits);
        REQUIRE(squeezed.has_value());
        CHECK(squeezed->data != kTinyPngBase64);
        REQUIRE(squeezed->resized.has_value());
        CHECK(squeezed->resized->from == dims(2, 2));
        CHECK(squeezed->resized->to == dims(1, 1));
    }
    SECTION("a limit no encoding can meet prepares nothing") {
        auto limits = ImageLimits{};
        limits.max_base64_bytes = 1;
        CHECK_FALSE(local_image_processor()->prepare(png, "image/png", limits).has_value());
    }
}

TEST_CASE("The local image processor orients, resizes, and converts like the frozen processor",
        "[durable][images][issue1090][spec]") {
    SECTION("EXIF orientation is applied before dimensions and resized bounds") {
        const auto jpeg = decode_base64(kPatternedJpegBase64);
        const auto prepared = local_image_processor()->prepare(jpeg, "image/jpeg", ImageLimits{});
        REQUIRE(prepared.has_value());
        const auto& expected = frozen_processor_case("jpeg");
        CHECK(prepared->mime_type == expected.at("mimeType").get_string());
        CHECK(prepared->converted_from.has_value() == !expected.at("convertedFrom").holds<std::nullptr_t>());
        CHECK(prepared->resized.has_value() == !expected.at("resized").holds<std::nullptr_t>());
        const auto output = image_dimensions(decode_base64(prepared->data), prepared->mime_type);
        REQUIRE(output.has_value());
        CHECK(output->width == static_cast<std::size_t>(expected.at("dimensions").at("width").get_number()));
        CHECK(output->height == static_cast<std::size_t>(expected.at("dimensions").at("height").get_number()));
        const auto pixels = decode_rgba(decode_base64(prepared->data));
        REQUIRE(pixels.has_value());
        for (const auto& sample : expected.at("quadrantSamples").get_array()) {
            const auto x = static_cast<std::size_t>(sample.at("x").get_number());
            const auto y = static_cast<std::size_t>(sample.at("y").get_number());
            const auto& rgba = sample.at("rgba").get_array();
            REQUIRE(rgba.size() == 4);
            REQUIRE(x < output->width);
            REQUIRE(y < output->height);
            const auto offset = (y * output->width + x) * 4;
            REQUIRE(offset + rgba.size() <= pixels->size());
            for (std::size_t channel = 0; channel < rgba.size(); ++channel) {
                CHECK(std::abs(static_cast<int>((*pixels)[offset + channel]) -
                               static_cast<int>(rgba.at(channel).get_number())) <= kPixelTolerance);
            }
        }
    }
    SECTION("an oriented WebP without the JPEG-first preference becomes PNG") {
        const auto webp = decode_base64(kOrientedWebpBase64);
        const auto prepared = local_image_processor()->prepare(webp, "image/webp", ImageLimits{});
        REQUIRE(prepared.has_value());
        const auto& expected = frozen_processor_case("webp");
        CHECK(prepared->mime_type == expected.at("mimeType").get_string());
        CHECK(prepared->converted_from == expected.at("convertedFrom").get_string());
        CHECK_FALSE(prepared->resized.has_value());
        const auto output = image_dimensions(decode_base64(prepared->data), prepared->mime_type);
        REQUIRE(output.has_value());
        CHECK(output->width == static_cast<std::size_t>(expected.at("dimensions").at("width").get_number()));
        CHECK(output->height == static_cast<std::size_t>(expected.at("dimensions").at("height").get_number()));
        CHECK(decode_rgba(decode_base64(prepared->data)).has_value());
    }
    SECTION("a limit smaller than the source resizes and reports both dimensions") {
        const auto png = tiny_png();
        auto limits = ImageLimits{};
        limits.max_width = 1;
        const auto prepared = local_image_processor()->prepare(png, "image/png", limits);
        REQUIRE(prepared.has_value());
        REQUIRE(prepared->resized.has_value());
        CHECK(prepared->resized->from == dims(2, 2));
        CHECK(prepared->resized->to == dims(1, 1));
        CHECK(prepared->mime_type == "image/png");
        CHECK_FALSE(prepared->converted_from.has_value());
        CHECK(dims_of(decode_base64(prepared->data), prepared->mime_type) == dims(1, 1));
    }
    SECTION("BMP converts to PNG") {
        const auto bmp = tests::tiny_bmp();
        const auto prepared = local_image_processor()->prepare(bmp, "image/bmp", ImageLimits{});
        REQUIRE(prepared.has_value());
        const auto& expected = frozen_processor_case("bmp");
        CHECK(prepared->mime_type == expected.at("mimeType").get_string());
        CHECK(prepared->converted_from == expected.at("convertedFrom").get_string());
        CHECK_FALSE(prepared->resized.has_value());
        const auto output = image_dimensions(decode_base64(prepared->data), prepared->mime_type);
        REQUIRE(output.has_value());
        CHECK(output->width == static_cast<std::size_t>(expected.at("dimensions").at("width").get_number()));
        CHECK(output->height == static_cast<std::size_t>(expected.at("dimensions").at("height").get_number()));
        const auto pixels = decode_rgba(decode_base64(prepared->data));
        REQUIRE(pixels.has_value());
        CHECK(*pixels == decode_base64(expected.at("pixelsBase64").get_string()));
    }
    SECTION("bytes that cannot be decoded prepare nothing") {
        const std::vector<std::uint8_t> damaged{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13};
        CHECK_FALSE(local_image_processor()->prepare(damaged, "image/png", ImageLimits{}).has_value());
    }
}

} // namespace cch::tests::durable
