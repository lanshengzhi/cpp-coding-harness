#include <cch/tui/Utils.hpp>

#include "support/PiTuiEvidence.hpp"

#include <cch/support/JsonValue.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <string_view>

using namespace cch::tui;
using namespace cch::support;

namespace {

[[nodiscard]] bool near(double first, double second, double tolerance = 1e-6) {
    return std::abs(first - second) <= tolerance;
}

[[nodiscard]] Color parsed_color(std::string_view value) {
    const auto result = parse_color(value);
    if (!result) std::terminate();
    return *result;
}

[[nodiscard]] cch::support::Expected<cch::support::JsonValue> color_evidence_scenario(std::string_view name) {
    auto fixture = cch::tests::read_pi_v130_audit_evidence("audit-1218", "color.json");
    if (!fixture) return std::unexpected(fixture.error());
    for (const auto& item : fixture->at("scenarios").get_array()) {
        if (item.at("name").get_string() == name) {
            return cch::support::JsonValue{item};
        }
    }
    return std::unexpected(
            cch::support::make_error(cch::support::ErrorCode::Validation, "Scenario not found: " + std::string(name)));
}

[[nodiscard]] Color json_to_color(const cch::support::JsonValue& val) {
    if (val.holds<std::string>()) {
        auto parsed = parse_color(val.get_string());
        if (!parsed) std::terminate();
        return *parsed;
    }
    if (val.holds<double>()) {
        auto parsed = parse_color(val.get_number());
        if (!parsed) std::terminate();
        return *parsed;
    }
    const auto kind = val.at("kind").get_string();
    if (kind == "oklch") {
        auto c = oklch_color(val.at("l").get_number(), val.at("c").get_number(), val.at("h").get_number());
        if (!c) std::terminate();
        return Color{*c};
    }
    if (kind == "rgb") {
        auto c = rgb_color(val.at("r").get_number(), val.at("g").get_number(), val.at("b").get_number());
        if (!c) std::terminate();
        return Color{*c};
    }
    auto c = indexed_color(static_cast<int>(val.at("index").get_number()));
    if (!c) std::terminate();
    return Color{*c};
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

TEST_CASE("pi-v1.3.0 color values construction and channels match frozen observation", "[tui][colors][issue1218]") {
    const auto scenario = color_evidence_scenario("color-values-and-construction");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");

    for (const auto& idx : expected.at("indexed").get_array()) {
        const int index = static_cast<int>(idx.at("index").get_number());
        const auto col = indexed_color(index);
        REQUIRE(col);
        CHECK(col->index == index);
        CHECK(col->kind == idx.at("kind").get_string());
    }

    for (const auto& item : expected.at("rgb").get_array()) {
        const double r = item.at("r").get_number();
        const double g = item.at("g").get_number();
        const double b = item.at("b").get_number();
        const auto col = rgb_color(r, g, b);
        REQUIRE(col);
        CHECK(col->red == r);
        CHECK(col->green == g);
        CHECK(col->blue == b);
        CHECK(col->r() == r);
        CHECK(col->g() == g);
        CHECK(col->b() == b);
        CHECK(col->kind == item.at("kind").get_string());

        const RgbColorValue val{.r = col->red, .g = col->green, .b = col->blue};
        CHECK(val.r == r);
        CHECK(val.g == g);
        CHECK(val.b == b);
        CHECK(val.kind == "rgb");
    }

    for (const auto& item : expected.at("oklch").get_array()) {
        const double l = item.at("l").get_number();
        const double c = item.at("c").get_number();
        const double h = item.at("h").get_number();
        const auto col = oklch_color(l, c, h);
        REQUIRE(col);
        CHECK(near(col->l, l, 1e-4));
        CHECK(near(col->c, c, 1e-4));
        CHECK(near(col->h, h, 1e-4));
        CHECK(col->kind == item.at("kind").get_string());

        const OklchChannels ch{.l = col->l, .c = col->c, .h = col->h};
        CHECK(near(ch.l, l, 1e-4));
        CHECK(near(ch.c, c, 1e-4));
        CHECK(near(ch.h, h, 1e-4));
    }
}

TEST_CASE("pi-v1.3.0 color parsing and hex formatting match frozen observation", "[tui][colors][issue1218]") {
    const auto scenario = color_evidence_scenario("color-parsing-and-hex");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");

    const auto check_group = [](const cch::support::JsonValue::array_t& rows) {
        for (const auto& item : rows) {
            const auto& input_val = item.at("input");
            cch::support::Expected<Color> parsed =
                    std::unexpected(cch::support::make_error(cch::support::ErrorCode::Validation, "unset"));
            if (input_val.holds<double>()) {
                parsed = parse_color(input_val.get_number());
            } else {
                parsed = parse_color(input_val.get_string());
            }
            REQUIRE(parsed);
            const auto hex = color_to_hex(*parsed);
            REQUIRE(hex);
            CHECK(*hex == item.at("hex").get_string());
        }
    };

    check_group(expected.at("hex").get_array());
    check_group(expected.at("indexed").get_array());
    check_group(expected.at("oklch").get_array());
    check_group(expected.at("okhsl").get_array());
}

TEST_CASE("pi-v1.3.0 okhsl colors and channels match frozen observation", "[tui][colors][issue1218]") {
    const auto scenario = color_evidence_scenario("okhsl-channels-and-conversion");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");

    for (const auto& item : expected.at("okhslColors").get_array()) {
        const double h = item.at("input").at("h").get_number();
        const double s = item.at("input").at("s").get_number();
        const double l = item.at("input").at("l").get_number();
        const auto rgb = okhsl_color(h, s, l);
        REQUIRE(rgb);
        CHECK(near(rgb->red, item.at("rgb").at("r").get_number(), 1.0));
        CHECK(near(rgb->green, item.at("rgb").at("g").get_number(), 1.0));
        CHECK(near(rgb->blue, item.at("rgb").at("b").get_number(), 1.0));

        const auto channels = color_to_okhsl(Color{*rgb});
        REQUIRE(channels);
        CHECK(near(channels->h, item.at("channels").at("h").get_number(), 1.0));
        CHECK(near(channels->s, item.at("channels").at("s").get_number(), 0.05));
        CHECK(near(channels->l, item.at("channels").at("l").get_number(), 0.02));
    }

    for (const auto& item : expected.at("colorsToOkhsl").get_array()) {
        const auto& in_val = item.at("input");
        Color col = in_val.holds<double>() ? *parse_color(in_val.get_number()) : *parse_color(in_val.get_string());
        const auto channels = color_to_okhsl(col);
        REQUIRE(channels);
        CHECK(near(channels->h, item.at("channels").at("h").get_number(), 1.0));
        CHECK(near(channels->s, item.at("channels").at("s").get_number(), 0.05));
        CHECK(near(channels->l, item.at("channels").at("l").get_number(), 0.02));
    }
}

TEST_CASE("pi-v1.3.0 color to rgb and oklch conversions match frozen observation", "[tui][colors][issue1218]") {
    const auto scenario = color_evidence_scenario("color-conversion-and-gamut-mapping");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");

    const auto& to_rgb = expected.at("toRgb");
    for (const auto& item : to_rgb.at("indexed").get_array()) {
        const auto idx = static_cast<int>(item.at("input").get_number());
        const auto rgb = color_to_rgb(Color{*indexed_color(idx)});
        REQUIRE(rgb);
        CHECK(*rgb == RgbColor{.red = item.at("rgb").at("r").get_number(),
                              .green = item.at("rgb").at("g").get_number(),
                              .blue = item.at("rgb").at("b").get_number()});
    }

    for (const auto& item : to_rgb.at("rgb").get_array()) {
        const auto& in = item.at("input");
        const auto rgb = color_to_rgb(
                Color{*rgb_color(in.at("r").get_number(), in.at("g").get_number(), in.at("b").get_number())});
        REQUIRE(rgb);
        CHECK(*rgb == RgbColor{.red = item.at("rgb").at("r").get_number(),
                              .green = item.at("rgb").at("g").get_number(),
                              .blue = item.at("rgb").at("b").get_number()});
    }

    for (const auto& item : to_rgb.at("oklch").get_array()) {
        const auto& in = item.at("input");
        const auto rgb = color_to_rgb(
                Color{*oklch_color(in.at("l").get_number(), in.at("c").get_number(), in.at("h").get_number())});
        REQUIRE(rgb);
        CHECK(*rgb == RgbColor{.red = item.at("rgb").at("r").get_number(),
                              .green = item.at("rgb").at("g").get_number(),
                              .blue = item.at("rgb").at("b").get_number()});
    }

    const auto& to_oklch = expected.at("toOklch");
    for (const auto& item : to_oklch.at("rgb").get_array()) {
        const auto& in = item.at("input");
        const auto oklch = color_to_oklch(
                Color{*rgb_color(in.at("r").get_number(), in.at("g").get_number(), in.at("b").get_number())});
        REQUIRE(oklch);
        CHECK(near(oklch->l, item.at("oklch").at("l").get_number(), 1e-4));
        CHECK(near(oklch->c, item.at("oklch").at("c").get_number(), 1e-4));
        CHECK(near(oklch->h, item.at("oklch").at("h").get_number(), 1e-4));
    }

    for (const auto& item : to_oklch.at("indexed").get_array()) {
        const auto idx = static_cast<int>(item.at("input").get_number());
        const auto oklch = color_to_oklch(Color{*indexed_color(idx)});
        REQUIRE(oklch);
        CHECK(near(oklch->l, item.at("oklch").at("l").get_number(), 1e-4));
        CHECK(near(oklch->c, item.at("oklch").at("c").get_number(), 1e-4));
        CHECK(near(oklch->h, item.at("oklch").at("h").get_number(), 1e-4));
    }

    for (const auto& item : to_oklch.at("oklch").get_array()) {
        const auto& in = item.at("input");
        const auto oklch = color_to_oklch(
                Color{*oklch_color(in.at("l").get_number(), in.at("c").get_number(), in.at("h").get_number())});
        REQUIRE(oklch);
        CHECK(near(oklch->l, item.at("oklch").at("l").get_number(), 1e-4));
        CHECK(near(oklch->c, item.at("oklch").at("c").get_number(), 1e-4));
        CHECK(near(oklch->h, item.at("oklch").at("h").get_number(), 1e-4));
    }
}

TEST_CASE("pi-v1.3.0 color mixing in oklch and srgb spaces matches frozen observation", "[tui][colors][issue1218]") {
    const auto scenario = color_evidence_scenario("color-mixing-and-spaces");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");

    for (const auto& item : expected.at("oklch").get_array()) {
        const auto first = json_to_color(item.at("first"));
        const auto second = json_to_color(item.at("second"));
        const double amount = item.at("amount").get_number();
        const auto mixed = mix_colors(first, second, amount, ColorMixSpace::Oklch);
        REQUIRE(mixed);
        const auto hex = color_to_hex(*mixed);
        REQUIRE(hex);
        CHECK(*hex == item.at("hex").get_string());
    }

    for (const auto& item : expected.at("srgb").get_array()) {
        const auto first = json_to_color(item.at("first"));
        const auto second = json_to_color(item.at("second"));
        const double amount = item.at("amount").get_number();
        const auto mixed = mix_colors(first, second, amount, ColorMixSpace::Srgb);
        REQUIRE(mixed);
        const auto hex = color_to_hex(*mixed);
        REQUIRE(hex);
        CHECK(*hex == item.at("hex").get_string());
    }
}

TEST_CASE("pi-v1.3.0 oklab to okhsl lightness matches frozen observation", "[tui][colors][issue1218]") {
    const auto scenario = color_evidence_scenario("oklab-to-okhsl-lightness");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");

    for (const auto& item : expected.at("values").get_array()) {
        const double in_val = item.at("input").get_number();
        const double exp_val = item.at("lightness").get_number();
        CHECK(near(oklab_to_okhsl_lightness(in_val), exp_val, 1e-8));
    }
}

TEST_CASE("pi-v1.3.0 text styling and foreground background ansi formatting match frozen observation",
        "[tui][colors][issue1218]") {
    const auto scenario = color_evidence_scenario("text-style-and-ansi-formatting");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");

    for (const auto& item : expected.at("foregroundAnsi").get_array()) {
        const auto& in = item.at("input");
        const auto mode =
                in.at("mode").get_string() == "truecolor" ? TerminalColorMode::TrueColor : TerminalColorMode::Xterm256;
        const auto col = json_to_color(in.at("color"));
        const auto ansi = foreground_ansi(col, mode);
        REQUIRE(ansi);
        CHECK(*ansi == item.at("ansi").get_string());
    }

    for (const auto& item : expected.at("backgroundAnsi").get_array()) {
        const auto& in = item.at("input");
        const auto mode =
                in.at("mode").get_string() == "truecolor" ? TerminalColorMode::TrueColor : TerminalColorMode::Xterm256;
        const auto col = json_to_color(in.at("color"));
        const auto ansi = background_ansi(col, mode);
        REQUIRE(ansi);
        CHECK(*ansi == item.at("ansi").get_string());
    }

    for (const auto& item : expected.at("styleText").get_array()) {
        const auto& in = item.at("input");
        const auto mode =
                in.at("mode").get_string() == "truecolor" ? TerminalColorMode::TrueColor : TerminalColorMode::Xterm256;
        const auto text = in.at("text").get_string();
        const auto& style_json = in.at("style");
        TextStyle style;
        if (style_json.get_object().contains("fg")) style.foreground = json_to_color(style_json.at("fg"));
        if (style_json.get_object().contains("bg")) style.background = json_to_color(style_json.at("bg"));
        if (style_json.get_object().contains("bold")) style.bold = style_json.at("bold").get_boolean();
        if (style_json.get_object().contains("dim")) style.dim = style_json.at("dim").get_boolean();
        if (style_json.get_object().contains("italic")) style.italic = style_json.at("italic").get_boolean();
        if (style_json.get_object().contains("underline")) style.underline = style_json.at("underline").get_boolean();
        if (style_json.get_object().contains("inverse")) style.inverse = style_json.at("inverse").get_boolean();
        if (style_json.get_object().contains("strikethrough"))
            style.strikethrough = style_json.at("strikethrough").get_boolean();

        const auto styled = style_text(text, style, mode);
        REQUIRE(styled);
        CHECK(*styled == item.at("output").get_string());
    }

    for (const auto& item : expected.at("styleTextWithAnsi").get_array()) {
        const auto& in = item.at("input");
        const auto text = in.at("text").get_string();
        std::optional<std::string> fg_ansi;
        std::optional<std::string> bg_ansi;
        if (in.get_object().contains("fgAnsi") && !in.at("fgAnsi").holds<cch::support::JsonValue::null_t>()) {
            fg_ansi = in.at("fgAnsi").get_string();
        }
        if (in.get_object().contains("bgAnsi") && !in.at("bgAnsi").holds<cch::support::JsonValue::null_t>()) {
            bg_ansi = in.at("bgAnsi").get_string();
        }
        TextAttributes attrs;
        if (in.get_object().contains("options")) {
            const auto& opts = in.at("options");
            if (opts.get_object().contains("bold")) attrs.bold = opts.at("bold").get_boolean();
            if (opts.get_object().contains("dim")) attrs.dim = opts.at("dim").get_boolean();
            if (opts.get_object().contains("italic")) attrs.italic = opts.at("italic").get_boolean();
            if (opts.get_object().contains("underline")) attrs.underline = opts.at("underline").get_boolean();
            if (opts.get_object().contains("inverse")) attrs.inverse = opts.at("inverse").get_boolean();
            if (opts.get_object().contains("strikethrough"))
                attrs.strikethrough = opts.at("strikethrough").get_boolean();
        }
        const auto styled = style_text_with_ansi(text, fg_ansi, bg_ansi, attrs);
        CHECK(styled == item.at("output").get_string());
    }
}

TEST_CASE("color construction rejects negative or out-of-range channels and non-finite values",
        "[tui][colors][issue1218][negative]") {
    CHECK_FALSE(indexed_color(-1));
    CHECK_FALSE(indexed_color(256));

    CHECK_FALSE(rgb_color(-1, 0, 0));
    CHECK_FALSE(rgb_color(0, 256, 0));
    CHECK_FALSE(rgb_color(0, 0, -0.5));
    CHECK_FALSE(rgb_color(0, 0, std::numeric_limits<double>::infinity()));
    CHECK_FALSE(rgb_color(0, 0, std::numeric_limits<double>::quiet_NaN()));

    CHECK_FALSE(oklch_color(-0.1, 0.1, 0));
    CHECK_FALSE(oklch_color(1.1, 0.1, 0));
    CHECK_FALSE(oklch_color(0.5, -0.1, 0));
    CHECK_FALSE(oklch_color(0.5, 0.1, std::numeric_limits<double>::quiet_NaN()));
}

TEST_CASE("color parsing rejects malformed formats and non-color strings without fallback",
        "[tui][colors][issue1218][negative]") {
    for (const auto value : {"",
                 "red",
                 "#12",
                 "#ggg",
                 " #fff",
                 "#12345",
                 "#1234567",
                 "oklch(101% 0.1 0)",
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
    CHECK_FALSE(parse_color(-1));
    CHECK_FALSE(parse_color(256));
    CHECK_FALSE(parse_color(1.5));
}

TEST_CASE(
        "color to hex formats exact two-digit lowercase hex with zero-padding", "[tui][colors][issue1218][negative]") {
    const auto col = rgb_color(0, 1, 15);
    REQUIRE(col);
    const auto hex = color_to_hex(Color{*col});
    REQUIRE(hex);
    CHECK(*hex == "#00010f");
    CHECK_FALSE(*hex == "#01f");
    CHECK_FALSE(*hex == "#00010F");
}

TEST_CASE("okhsl color construction and channel conversion reject out-of-range saturation and lightness",
        "[tui][colors][issue1218][negative]") {
    CHECK_FALSE(okhsl_color(0, -0.1, 0.5));
    CHECK_FALSE(okhsl_color(0, 1.1, 0.5));
    CHECK_FALSE(okhsl_color(0, 0.5, -0.1));
    CHECK_FALSE(okhsl_color(0, 0.5, 1.1));
    CHECK_FALSE(okhsl_color(std::numeric_limits<double>::quiet_NaN(), 0.5, 0.5));

    const auto gray = rgb_color(128, 128, 128);
    REQUIRE(gray);
    const auto channels = color_to_okhsl(Color{*gray});
    REQUIRE(channels);
    CHECK(near(channels->s, 0.0, 1e-4));
    CHECK(near(channels->h, 0.0, 1e-4));
}

TEST_CASE("color conversion gamut maps out-of-gamut chroma without distorting hue",
        "[tui][colors][issue1218][boundary]") {
    const auto white = oklch_color(1.0, 0.3, 150);
    REQUIRE(white);
    const auto white_rgb = color_to_rgb(Color{*white});
    REQUIRE(white_rgb);
    CHECK(*white_rgb == RgbColor{.red = 255, .green = 255, .blue = 255});

    const auto black = oklch_color(0.0, 0.3, 150);
    REQUIRE(black);
    const auto black_rgb = color_to_rgb(Color{*black});
    REQUIRE(black_rgb);
    CHECK(*black_rgb == RgbColor{.red = 0, .green = 0, .blue = 0});

    const auto red = rgb_color(255, 0, 0);
    REQUIRE(red);
    const auto oklch = color_to_oklch(Color{*red});
    REQUIRE(oklch);
    CHECK(near(oklch->h, 29.23388, 1e-4));
}

TEST_CASE("color mixing enforces unit bounds and separates polar oklch from cartesian srgb interpolation",
        "[tui][colors][issue1218][boundary]") {
    const auto red = *parse_color("#ff0000");
    const auto blue = *parse_color("#0000ff");

    CHECK_FALSE(mix_colors(red, blue, -0.01));
    CHECK_FALSE(mix_colors(red, blue, 1.01));
    CHECK_FALSE(mix_colors(red, blue, std::numeric_limits<double>::quiet_NaN()));

    const auto srgb = mix_colors(red, blue, 0.5, ColorMixSpace::Srgb);
    const auto oklch = mix_colors(red, blue, 0.5, ColorMixSpace::Oklch);
    REQUIRE(srgb);
    REQUIRE(oklch);
    CHECK(std::get<RgbColor>(*srgb) == RgbColor{.red = 127.5, .green = 0, .blue = 127.5});
    const auto oklch_rgb = color_to_rgb(*oklch);
    REQUIRE(oklch_rgb);
    CHECK(*oklch_rgb != std::get<RgbColor>(*srgb));
    CHECK(*color_to_hex(*oklch) == "#b200b8");
}

TEST_CASE("oklab to okhsl lightness diverges from linear and quadratic approximations",
        "[tui][colors][issue1218][boundary]") {
    const double evaluated = oklab_to_okhsl_lightness(0.5);
    CHECK_FALSE(near(evaluated, 0.5, 1e-2));
    CHECK_FALSE(near(evaluated, std::sqrt(0.5), 1e-2));
    CHECK_FALSE(near(evaluated, 0.25, 1e-2));
}

TEST_CASE("foreground and background ANSI format distinct codes for 256color and truecolor modes",
        "[tui][colors][issue1218][boundary]") {
    const auto red = *rgb_color(255, 0, 0);
    const auto fg_true = foreground_ansi(Color{red}, TerminalColorMode::TrueColor);
    const auto fg_256 = foreground_ansi(Color{red}, TerminalColorMode::Xterm256);
    REQUIRE(fg_true);
    REQUIRE(fg_256);
    CHECK(*fg_true == "\x1b[38;2;255;0;0m");
    CHECK(*fg_256 == "\x1b[38;5;196m");

    const auto bg_true = background_ansi(Color{red}, TerminalColorMode::TrueColor);
    const auto bg_256 = background_ansi(Color{red}, TerminalColorMode::Xterm256);
    REQUIRE(bg_true);
    REQUIRE(bg_256);
    CHECK(*bg_true == "\x1b[48;2;255;0;0m");
    CHECK(*bg_256 == "\x1b[48;5;196m");

    const auto indexed = *indexed_color(9);
    CHECK(*foreground_ansi(Color{indexed}, TerminalColorMode::TrueColor) == "\x1b[38;5;9m");
    CHECK(*foreground_ansi(Color{indexed}, TerminalColorMode::Xterm256) == "\x1b[38;5;9m");
}

TEST_CASE("style_text and style_text_with_ansi preserve unstyled text and enforce SGR reset ordering",
        "[tui][colors][issue1218][boundary]") {
    CHECK(*style_text("Plain", {}, TerminalColorMode::TrueColor) == "Plain");
    CHECK(style_text_with_ansi("Plain", std::nullopt, std::nullopt, TextAttributes{}) == "Plain");

    const TextStyle style{
            .foreground = Color{*rgb_color(1, 2, 3)},
            .background = Color{*indexed_color(4)},
            .bold = true,
            .dim = true,
            .italic = true,
            .underline = true,
            .inverse = true,
            .strikethrough = true,
    };
    const auto styled = style_text("X", style, TerminalColorMode::TrueColor);
    REQUIRE(styled);
    CHECK(styled->ends_with("\x1b[29m\x1b[27m\x1b[24m\x1b[23m\x1b[22m\x1b[49m\x1b[39m"));
}
