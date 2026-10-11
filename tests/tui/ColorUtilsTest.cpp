#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <exception>
#include <string_view>

using namespace cch::tui;

namespace {

[[nodiscard]] bool near(double first, double second, double tolerance = 1e-6) {
    return std::abs(first - second) <= tolerance;
}

[[nodiscard]] Color parsed_color(std::string_view value) {
    const auto result = parse_color(value);
    if (!result) std::terminate();
    return *result;
}

} // namespace

TEST_CASE("OKLCH conversion round trips representative sRGB colors", "[tui][utils][color][issue989][spec]") {
    for (const auto rgb : {RgbColor{.red = 255, .green = 0, .blue = 0},
                 RgbColor{.red = 12, .green = 128, .blue = 240},
                 RgbColor{.red = 96, .green = 96, .blue = 96}}) {
        const auto oklch = color_to_oklch(Color{rgb});
        REQUIRE(oklch);
        const auto round_trip = color_to_rgb(Color{*oklch});
        REQUIRE(round_trip);
        CHECK(std::abs(round_trip->red - rgb.red) <= 1);
        CHECK(std::abs(round_trip->green - rgb.green) <= 1);
        CHECK(std::abs(round_trip->blue - rgb.blue) <= 1);
    }
}

TEST_CASE("OKHSL conversion round trips representative sRGB colors", "[tui][utils][color][issue989][spec]") {
    for (const auto rgb : {RgbColor{.red = 255, .green = 0, .blue = 0},
                 RgbColor{.red = 12, .green = 128, .blue = 240},
                 RgbColor{.red = 96, .green = 96, .blue = 96}}) {
        const auto channels = color_to_okhsl(Color{rgb});
        REQUIRE(channels);
        const auto round_trip = okhsl_to_rgb(channels->h, channels->s, channels->l);
        REQUIRE(round_trip);
        CHECK(std::abs(round_trip->red - rgb.red) <= 1);
        CHECK(std::abs(round_trip->green - rgb.green) <= 1);
        CHECK(std::abs(round_trip->blue - rgb.blue) <= 1);
    }
}

TEST_CASE("OKHSL lightness follows the frozen Oklab lightness conversion", "[tui][utils][color][issue989][spec]") {
    const auto oklch = color_to_oklch(Color{RgbColor{.red = 128, .green = 128, .blue = 128}});
    const auto okhsl = color_to_okhsl(Color{RgbColor{.red = 128, .green = 128, .blue = 128}});
    REQUIRE(oklch);
    REQUIRE(okhsl);
    constexpr double k1 = 0.206;
    constexpr double k2 = 0.03;
    constexpr double k3 = (1 + k1) / (1 + k2);
    const auto term = k3 * oklch->l - k1;
    const auto expected = 0.5 * (term + std::sqrt(term * term + 4 * k2 * k3 * oklch->l));
    CHECK(near(okhsl->l, expected));
    CHECK_FALSE(near(okhsl->l, oklch->l, 1e-3));
}

TEST_CASE("OKLCH mixing wraps hue and preserves chromatic hue for achromatic endpoints",
        "[tui][utils][color][issue989][spec]") {
    const auto first = oklch_color(0.7, 0.1, 350);
    const auto second = oklch_color(0.7, 0.1, 10);
    REQUIRE(first);
    REQUIRE(second);
    const auto wrapped = mix_colors(Color{*first}, Color{*second}, 0.5);
    REQUIRE(wrapped);
    REQUIRE(std::holds_alternative<OklchColorValue>(*wrapped));
    CHECK(near(std::get<OklchColorValue>(*wrapped).h, 0));

    const auto gray = parsed_color("#808080");
    const auto chromatic = oklch_color(0.7, 0.12, 217);
    REQUIRE(chromatic);
    const auto mixed = mix_colors(gray, Color{*chromatic}, 0.5);
    REQUIRE(mixed);
    REQUIRE(std::holds_alternative<OklchColorValue>(*mixed));
    CHECK(near(std::get<OklchColorValue>(*mixed).h, chromatic->h));

    const auto achromatic_second = mix_colors(Color{*chromatic}, gray, 0.5);
    REQUIRE(achromatic_second);
    REQUIRE(std::holds_alternative<OklchColorValue>(*achromatic_second));
    CHECK(near(std::get<OklchColorValue>(*achromatic_second).h, chromatic->h));
}

TEST_CASE("OKLCH conversion matches the frozen red endpoint", "[tui][utils][color][issue989][spec]") {
    const auto red = parse_color("#ff0000");
    REQUIRE(red);
    const auto oklch = color_to_oklch(*red);
    REQUIRE(oklch);
    CHECK(near(oklch->l, 0.6279553639214309, 1e-9));
    CHECK(near(oklch->c, 0.2576833038053606, 1e-9));
    CHECK(near(oklch->h, 29.23388027962784, 1e-9));
}

TEST_CASE("sRGB mixing interpolates encoded endpoints while OKLCH mixing stays polar",
        "[tui][utils][color][issue989][spec]") {
    const auto red = parsed_color("#ff0000");
    const auto blue = parsed_color("#0000ff");
    const auto srgb = mix_colors(red, blue, 0.5, ColorMixSpace::Srgb);
    const auto oklch = mix_colors(red, blue, 0.5, ColorMixSpace::Oklch);
    REQUIRE(srgb);
    REQUIRE(oklch);
    REQUIRE(std::holds_alternative<RgbColor>(*srgb));
    REQUIRE(std::holds_alternative<OklchColorValue>(*oklch));
    CHECK(std::get<RgbColor>(*srgb) == RgbColor{.red = 127.5, .green = 0, .blue = 127.5});
    const auto oklch_rgb = color_to_rgb(*oklch);
    REQUIRE(oklch_rgb);
    CHECK(*oklch_rgb != std::get<RgbColor>(*srgb));
    const auto oklch_hex = color_to_hex(*oklch);
    REQUIRE(oklch_hex);
    CHECK(*oklch_hex == "#b200b8");
    CHECK_FALSE(mix_colors(red, blue, 1.1));
}

TEST_CASE("OKLCH and OKHSL strings parse numbers percentages and degree suffixes",
        "[tui][utils][color][issue989][spec]") {
    const auto oklch = parse_color("oklch(80% 0.15 150deg)");
    REQUIRE(oklch);
    REQUIRE(std::holds_alternative<OklchColorValue>(*oklch));
    const auto& channels = std::get<OklchColorValue>(*oklch);
    CHECK(near(channels.l, 0.8));
    CHECK(near(channels.c, 0.15));
    CHECK(near(channels.h, 150));

    const auto decimal_oklch = parse_color("OKLCH(0.8 0.15 510)");
    REQUIRE(decimal_oklch);
    CHECK(near(std::get<OklchColorValue>(*decimal_oklch).h, 150));

    const auto okhsl = parse_color("okhsl(150 50% 80%)");
    REQUIRE(okhsl);
    REQUIRE(std::holds_alternative<RgbColor>(*okhsl));
    const auto okhsl_rgb = color_to_rgb(*okhsl);
    REQUIRE(okhsl_rgb);
    const auto channels_round_trip = color_to_okhsl(*okhsl);
    REQUIRE(channels_round_trip);
    CHECK(near(channels_round_trip->h, 150, 0.5));
    CHECK(near(channels_round_trip->s, 0.5, 0.02));
    CHECK(near(channels_round_trip->l, 0.8, 0.01));

    const auto decimal_okhsl = parse_color("okhsl(150deg 0.5 0.8)");
    REQUIRE(decimal_okhsl);
    const auto decimal_okhsl_rgb = color_to_rgb(*decimal_okhsl);
    REQUIRE(decimal_okhsl_rgb);
    CHECK(*decimal_okhsl_rgb == *okhsl_rgb);
}

TEST_CASE("OKLCH and OKHSL parsers reject malformed or out of range values", "[tui][utils][color][issue989][spec]") {
    for (const auto value : {"oklch(101% 0.1 0)",
                 "oklch(0.5 -0.1 0)",
                 "oklch(0.5 0.1 0rad)",
                 "oklch(0.5 0.1)",
                 "okhsl(0 101% 50%)",
                 "okhsl(0 50% 101%)",
                 "okhsl(1turn 50% 50%)",
                 "okhsl(0, 50%, 50%)",
                 "okhsl(0 50%)"}) {
        CHECK_FALSE(parse_color(value));
    }
    CHECK_FALSE(okhsl_to_rgb(0, 1.1, 0.5));
    CHECK_FALSE(oklch_color(1.1, 0.1, 0));
}

TEST_CASE("color_to_hex converts OKLCH and OKHSL colors to lowercase sRGB hex", "[tui][utils][color][issue989][spec]") {
    const auto red = parse_color("okhsl(29.23 100% 56.8%)");
    REQUIRE(red);
    const auto hex = color_to_hex(*red);
    REQUIRE(hex);
    CHECK(*hex == "#ff0000");

    const auto mapped = oklch_color(1, 0.3, 150);
    REQUIRE(mapped);
    const auto mapped_hex = color_to_hex(Color{*mapped});
    REQUIRE(mapped_hex);
    CHECK(*mapped_hex == "#ffffff");
}
