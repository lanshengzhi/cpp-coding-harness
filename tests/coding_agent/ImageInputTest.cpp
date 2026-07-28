#include "coding_agent/ImageInput.hpp"
#include "support/ImageFixture.hpp"

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

using namespace cch;

TEST_CASE(
    "Image input resizing follows baseline bounds and emits the coordinate hint",
    "[coding_agent][image-input][issue63]") {
    const auto bytes = tests::decode_base64(tests::kTinyPngBase64);
    coding_agent::ImageProcessingLimits limits;
    limits.max_width = 1;
    limits.max_height = 1;

    const auto processed = coding_agent::process_image_input(bytes, "image/png", limits);

    REQUIRE(processed);
    REQUIRE(processed->image.has_value());
    REQUIRE(processed->hints.size() == 1);
    CHECK(processed->hints[0] ==
        "[Image: original 2x2, displayed at 1x1. Multiply coordinates by 2.00 to map to original image.]");
    CHECK(processed->image->mime_type == "image/png");
    CHECK(processed->image->data.size() < limits.max_base64_bytes);
}

TEST_CASE(
    "Oversized WebP input is decoded and resized with the same baseline policy",
    "[coding_agent][image-input][webp][issue63]") {
    const auto bytes = tests::decode_base64(tests::kTinyWebpBase64);
    coding_agent::ImageProcessingLimits limits;
    limits.max_width = 1;
    limits.max_height = 1;

    const auto processed = coding_agent::process_image_input(bytes, "image/webp", limits);

    REQUIRE(processed);
    REQUIRE(processed->image.has_value());
    REQUIRE(processed->hints.size() == 1);
    CHECK(processed->hints[0] ==
        "[Image: original 2x2, displayed at 1x1. Multiply coordinates by 2.00 to map to original image.]");
    CHECK(processed->image->mime_type == "image/png");
}

TEST_CASE(
    "JPEG and WebP EXIF orientation is applied before dimensions and coordinate hints",
    "[coding_agent][image-input][orientation][issue63]") {
    const std::array fixtures{
        std::pair{tests::kOrientedJpegBase64, std::string_view{"image/jpeg"}},
        std::pair{tests::kOrientedWebpBase64, std::string_view{"image/webp"}},
    };
    coding_agent::ImageProcessingLimits limits;
    limits.max_width = 1;
    limits.max_height = 3;

    for (const auto& [fixture, mime_type] : fixtures) {
        const auto processed = coding_agent::process_image_input(
            tests::decode_base64(fixture),
            std::string(mime_type),
            limits);
        REQUIRE(processed);
        REQUIRE(processed->image.has_value());
        REQUIRE(processed->hints.size() == 1);
        CHECK(processed->hints[0] ==
            "[Image: original 2x3, displayed at 1x2. Multiply coordinates by 2.00 to map to original image.]");
    }
}

TEST_CASE(
    "Invalid WebP data is omitted instead of becoming provider image content",
    "[coding_agent][image-input][webp][issue63]") {
    const std::array<std::uint8_t, 16> invalid{
        'R', 'I', 'F', 'F', 0xff, 0xff, 0xff, 0x7f,
        'W', 'E', 'B', 'P', 'V', 'P', '8', ' ',
    };

    CHECK(coding_agent::sniff_supported_image_mime_type(invalid) == "image/webp");
    const auto processed = coding_agent::process_image_input(invalid, "image/webp");
    REQUIRE(processed);
    CHECK_FALSE(processed->image.has_value());
    REQUIRE(processed->omission.has_value());
    CHECK(processed->omission->find("Image omitted") != std::string::npos);
}
