#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

struct Vector3 {
    double x;
    double y;
    double z;
};

using Matrix3 = std::array<Vector3, 3>;

constexpr Matrix3 kLinearSrgbToLms{{
        {0.4122214694707629, 0.5363325372617349, 0.0514459932675022},
        {0.2119034958178251, 0.6806995506452344, 0.1073969535369405},
        {0.0883024591900564, 0.2817188391361215, 0.6299787016738222},
}};
constexpr Matrix3 kLmsToLab{{
        {0.210454268309314, 0.793617774702305, -0.0040720430116193},
        {1.9779985324311684, -2.42859224204858, 0.450593709617411},
        {0.0259040424655478, 0.7827717124575296, -0.8086757549230774},
}};
constexpr Matrix3 kLabToLms{{
        {1, 0.3963377773761749, 0.2158037573099136},
        {1, -0.1055613458156586, -0.0638541728258133},
        {1, -0.0894841775298119, -1.2914855480194092},
}};
constexpr Matrix3 kLmsToLinearSrgb{{
        {4.0767416360759583, -3.3077115392580629, 0.2309699031821043},
        {-1.2684379732850315, 2.6097573492876882, -0.341319376002657},
        {-0.0041960761386756, -0.7034186179359362, 1.7076146940746117},
}};
constexpr std::array<std::array<double, 2>, 3> kSaturationFitDirection{{
        {-1.8817031, -0.80936501},
        {1.8144408, -1.19445267},
        {0.13110758, 1.81333971},
}};
constexpr std::array<std::array<double, 5>, 3> kSaturationFit{{
        {1.19086277, 1.76576728, 0.59662641, 0.75515197, 0.56771245},
        {0.73956515, -0.45954404, 0.08285427, 0.12541073, -0.14503204},
        {1.35733652, -0.00915799, -1.1513021, -0.50559606, 0.00692167},
}};
constexpr double kK1 = 0.206;
constexpr double kK2 = 0.03;
constexpr double kK3 = (1 + kK1) / (1 + kK2);
constexpr std::array<RgbColor, 16> kBasicColors{{
        {.red = 0, .green = 0, .blue = 0},
        {.red = 128, .green = 0, .blue = 0},
        {.red = 0, .green = 128, .blue = 0},
        {.red = 128, .green = 128, .blue = 0},
        {.red = 0, .green = 0, .blue = 128},
        {.red = 128, .green = 0, .blue = 128},
        {.red = 0, .green = 128, .blue = 128},
        {.red = 192, .green = 192, .blue = 192},
        {.red = 128, .green = 128, .blue = 128},
        {.red = 255, .green = 0, .blue = 0},
        {.red = 0, .green = 255, .blue = 0},
        {.red = 255, .green = 255, .blue = 0},
        {.red = 0, .green = 0, .blue = 255},
        {.red = 255, .green = 0, .blue = 255},
        {.red = 0, .green = 255, .blue = 255},
        {.red = 255, .green = 255, .blue = 255},
}};
constexpr std::array<double, 6> kColorCubeValues{0, 95, 135, 175, 215, 255};
constexpr std::array<double, 24> kGrayValues{
        8,
        18,
        28,
        38,
        48,
        58,
        68,
        78,
        88,
        98,
        108,
        118,
        128,
        138,
        148,
        158,
        168,
        178,
        188,
        198,
        208,
        218,
        228,
        238,
};

[[nodiscard]] support::Error invalid_color(std::string message) {
    return support::make_error(support::ErrorCode::Validation, std::move(message));
}

[[nodiscard]] Vector3 multiply(const Matrix3& matrix, Vector3 vector) {
    return {
            matrix[0].x * vector.x + matrix[0].y * vector.y + matrix[0].z * vector.z,
            matrix[1].x * vector.x + matrix[1].y * vector.y + matrix[1].z * vector.z,
            matrix[2].x * vector.x + matrix[2].y * vector.y + matrix[2].z * vector.z,
    };
}

[[nodiscard]] Vector3 oklab_to_linear_srgb(Vector3 lab) {
    const auto lms = multiply(kLabToLms, lab);
    return multiply(kLmsToLinearSrgb, {lms.x * lms.x * lms.x, lms.y * lms.y * lms.y, lms.z * lms.z * lms.z});
}

[[nodiscard]] double srgb_to_linear(double value) {
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double linear_to_srgb(double value) {
    return value > 0.0031308 ? 1.055 * std::pow(value, 1 / 2.4) - 0.055 : 12.92 * value;
}

[[nodiscard]] Vector3 rgb_to_oklab(const RgbColor& rgb) {
    const auto lms = multiply(kLinearSrgbToLms,
            {srgb_to_linear(rgb.red / 255), srgb_to_linear(rgb.green / 255), srgb_to_linear(rgb.blue / 255)});
    const auto roots = Vector3{std::cbrt(lms.x), std::cbrt(lms.y), std::cbrt(lms.z)};
    return multiply(kLmsToLab, roots);
}

[[nodiscard]] RgbColor linear_srgb_to_rgb(Vector3 linear) {
    const auto channel = [](double value) { return std::round(std::clamp(linear_to_srgb(value), 0.0, 1.0) * 255); };
    return {.red = channel(linear.x), .green = channel(linear.y), .blue = channel(linear.z)};
}

[[nodiscard]] Vector3 lms_slopes(double a, double b) {
    return {
            kLabToLms[0].y * a + kLabToLms[0].z * b,
            kLabToLms[1].y * a + kLabToLms[1].z * b,
            kLabToLms[2].y * a + kLabToLms[2].z * b,
    };
}

[[nodiscard]] double max_saturation(double a, double b) {
    std::size_t channel = 0;
    while (channel < 2 && kSaturationFitDirection[channel][0] * a + kSaturationFitDirection[channel][1] * b <= 1) {
        ++channel;
    }
    const auto& fit = kSaturationFit[channel];
    const auto saturation = fit[0] + fit[1] * a + fit[2] * b + fit[3] * a * a + fit[4] * a * b;
    const auto slopes = lms_slopes(a, b);
    const auto base = Vector3{1 + saturation * slopes.x, 1 + saturation * slopes.y, 1 + saturation * slopes.z};
    const auto& weights = kLmsToLinearSrgb[channel];
    const auto dot = [&](Vector3 value) { return weights.x * value.x + weights.y * value.y + weights.z * value.z; };
    const auto f = dot({base.x * base.x * base.x, base.y * base.y * base.y, base.z * base.z * base.z});
    const auto f1 =
            dot({3 * slopes.x * base.x * base.x, 3 * slopes.y * base.y * base.y, 3 * slopes.z * base.z * base.z});
    const auto f2 =
            dot({6 * slopes.x * slopes.x * base.x, 6 * slopes.y * slopes.y * base.y, 6 * slopes.z * slopes.z * base.z});
    return saturation - (f * f1) / (f1 * f1 - 0.5 * f * f2);
}

[[nodiscard]] std::array<double, 2> cusp(double a, double b) {
    const auto saturation = max_saturation(a, b);
    const auto linear = oklab_to_linear_srgb({1, saturation * a, saturation * b});
    const auto lightness = std::cbrt(1 / std::max({linear.x, linear.y, linear.z}));
    return {lightness, lightness * saturation};
}

[[nodiscard]] double max_chroma(double a, double b, double lightness, const std::array<double, 2>& peak) {
    const auto [cusp_lightness, cusp_chroma] = peak;
    if (lightness <= cusp_lightness) return (cusp_chroma * lightness) / cusp_lightness;

    const auto t = (cusp_chroma * (lightness - 1)) / (cusp_lightness - 1);
    const auto slopes = lms_slopes(a, b);
    const auto lms = Vector3{lightness + t * slopes.x, lightness + t * slopes.y, lightness + t * slopes.z};
    const auto cubes = Vector3{lms.x * lms.x * lms.x, lms.y * lms.y * lms.y, lms.z * lms.z * lms.z};
    const auto first =
            Vector3{3 * slopes.x * lms.x * lms.x, 3 * slopes.y * lms.y * lms.y, 3 * slopes.z * lms.z * lms.z};
    const auto second =
            Vector3{6 * slopes.x * slopes.x * lms.x, 6 * slopes.y * slopes.y * lms.y, 6 * slopes.z * slopes.z * lms.z};
    const auto dot = [](Vector3 row, Vector3 value) { return row.x * value.x + row.y * value.y + row.z * value.z; };
    double step = std::numeric_limits<double>::max();
    for (const auto& row : kLmsToLinearSrgb) {
        const auto f = dot(row, cubes) - 1;
        const auto f1 = dot(row, first);
        const auto f2 = dot(row, second);
        const auto u = f1 / (f1 * f1 - 0.5 * f * f2);
        if (u >= 0) step = std::min(step, -f * u);
    }
    return t + step;
}

[[nodiscard]] std::array<double, 3> chroma_stops(double lightness, double a, double b) {
    const auto peak = cusp(a, b);
    const auto c_max = max_chroma(a, b, lightness, peak);
    const auto k = c_max / std::min(lightness * (peak[1] / peak[0]), (1 - lightness) * (peak[1] / (1 - peak[0])));
    const auto mid_s =
            0.11516993 + 1 / (7.4477897 + 4.1590124 * b +
                                     a * (-2.19557347 + 1.75198401 * b +
                                                 a * (-2.13704948 - 10.02301043 * b +
                                                             a * (-4.24894561 + 5.38770819 * b + 4.69891013 * a))));
    const auto mid_t =
            0.11239642 + 1 / (1.6132032 - 0.68124379 * b +
                                     a * (0.40370612 + 0.90148123 * b +
                                                 a * (-0.27087943 + 0.6122399 * b +
                                                             a * (0.00299215 - 0.45399568 * b - 0.14661872 * a))));
    const auto c_mid =
            0.9 * k *
            std::sqrt(std::sqrt(1 / (1 / std::pow(lightness * mid_s, 4) + 1 / std::pow((1 - lightness) * mid_t, 4))));
    const auto c0 = std::sqrt(1 / (1 / std::pow(lightness * 0.4, 2) + 1 / std::pow((1 - lightness) * 0.8, 2)));
    return {c0, c_mid, c_max};
}

[[nodiscard]] double normalize_hue(double hue) { return std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0); }

[[nodiscard]] double oklab_to_okhsl_lightness(double value) {
    const auto term = kK3 * value - kK1;
    return 0.5 * (term + std::sqrt(term * term + 4 * kK2 * kK3 * value));
}

[[nodiscard]] double okhsl_to_oklab_lightness(double value) {
    return (value * value + kK1 * value) / (kK3 * (value + kK2));
}

[[nodiscard]] RgbColor okhsl_to_rgb_unchecked(double hue, double saturation, double lightness) {
    const auto lab_lightness = okhsl_to_oklab_lightness(lightness);
    Vector3 lab{lab_lightness, 0, 0};
    if (lab_lightness > 0 && lab_lightness < 1 && saturation > 0) {
        const auto angle = 2 * std::numbers::pi * (normalize_hue(hue) / 360);
        const auto a = std::cos(angle);
        const auto b = std::sin(angle);
        const auto [c0, c_mid, c_max] = chroma_stops(lab_lightness, a, b);
        double chroma = 0;
        if (saturation < 0.8) {
            const auto t = 1.25 * saturation;
            const auto k1 = 0.8 * c0;
            chroma = (t * k1) / (1 - (1 - k1 / c_mid) * t);
        } else {
            const auto t = 5 * (saturation - 0.8);
            const auto k1 = (0.2 * c_mid * c_mid * 1.25 * 1.25) / c0;
            chroma = c_mid + (t * k1) / (1 - (1 - k1 / (c_max - c_mid)) * t);
        }
        lab = {lab_lightness, chroma * a, chroma * b};
    }
    return linear_srgb_to_rgb(oklab_to_linear_srgb(lab));
}

[[nodiscard]] OkhslChannels rgb_to_okhsl_unchecked(const RgbColor& rgb) {
    const auto lab = rgb_to_oklab(rgb);
    const auto chroma = std::hypot(lab.y, lab.z);
    const auto lightness = oklab_to_okhsl_lightness(lab.x);
    if (chroma < 1e-9 || lightness <= 0 || lightness >= 1) return {.h = 0, .s = 0, .l = lightness};

    const auto hue = std::fmod(std::atan2(lab.z, lab.y) * 180 / std::numbers::pi + 360, 360);
    const auto stops = chroma_stops(lab.x, lab.y / chroma, lab.z / chroma);
    const auto [c0, c_mid, c_max] = stops;
    double saturation = 0;
    if (chroma < c_mid) {
        const auto k1 = 0.8 * c0;
        saturation = 0.8 * (chroma / (k1 + (1 - k1 / c_mid) * chroma));
    } else {
        const auto k1 = (0.2 * c_mid * c_mid * 1.25 * 1.25) / c0;
        const auto offset = chroma - c_mid;
        saturation = 0.8 + 0.2 * (offset / (k1 + (1 - k1 / (c_max - c_mid)) * offset));
    }
    return {.h = hue, .s = std::clamp(saturation, 0.0, 1.0), .l = lightness};
}

[[nodiscard]] bool is_in_srgb_gamut(Vector3 linear) {
    constexpr double kEpsilon = 1e-7;
    return linear.x >= -kEpsilon && linear.x <= 1 + kEpsilon && linear.y >= -kEpsilon && linear.y <= 1 + kEpsilon &&
           linear.z >= -kEpsilon && linear.z <= 1 + kEpsilon;
}

[[nodiscard]] RgbColor oklch_to_rgb(const OklchColorValue& color) {
    const auto radians = color.h * std::numbers::pi / 180;
    const auto a = std::cos(radians);
    const auto b = std::sin(radians);
    const auto at_chroma = [&](double chroma) { return oklab_to_linear_srgb({color.l, chroma * a, chroma * b}); };
    const auto direct = at_chroma(color.c);
    if (is_in_srgb_gamut(direct)) return linear_srgb_to_rgb(direct);

    auto linear = at_chroma(0);
    double low = 0;
    double high = color.c;
    for (int iteration = 0; iteration < 20; ++iteration) {
        const auto chroma = (low + high) / 2;
        const auto candidate = at_chroma(chroma);
        if (is_in_srgb_gamut(candidate)) {
            low = chroma;
            linear = candidate;
        } else {
            high = chroma;
        }
    }
    return linear_srgb_to_rgb(linear);
}

[[nodiscard]] std::optional<std::uint8_t> parse_hex_byte(std::string_view value) {
    if (value.empty()) return std::nullopt;
    unsigned int parsed = 0;
    for (const char character : value) {
        unsigned int digit = 0;
        if (character >= '0' && character <= '9')
            digit = static_cast<unsigned int>(character - '0');
        else if (character >= 'a' && character <= 'f')
            digit = static_cast<unsigned int>(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F')
            digit = static_cast<unsigned int>(character - 'A' + 10);
        else
            return std::nullopt;
        parsed = parsed * 16 + digit;
    }
    return static_cast<std::uint8_t>(parsed);
}

[[nodiscard]] bool is_space(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] char lower_ascii(char value) {
    if (value >= 'A' && value <= 'Z') return static_cast<char>(value - 'A' + 'a');
    return value;
}

struct ParsedNumber {
    double value;
    std::size_t end;
};

[[nodiscard]] std::optional<ParsedNumber> parse_number(std::string_view value, std::size_t position) {
    const auto start = position;
    if (position < value.size() && (value[position] == '+' || value[position] == '-')) ++position;
    std::size_t digits = 0;
    while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
        ++position;
        ++digits;
    }
    if (position < value.size() && value[position] == '.') {
        ++position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            ++position;
            ++digits;
        }
    }
    if (digits == 0) return std::nullopt;
    if (position < value.size() && (value[position] == 'e' || value[position] == 'E')) {
        ++position;
        if (position < value.size() && (value[position] == '+' || value[position] == '-')) ++position;
        const auto exponent_start = position;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9')
            ++position;
        if (position == exponent_start) return std::nullopt;
    }
    double parsed = 0;
    const auto parse_start = start < position && value[start] == '+' ? start + 1 : start;
    const auto result = std::from_chars(value.data() + parse_start, value.data() + position, parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + position || !std::isfinite(parsed))
        return std::nullopt;
    return ParsedNumber{.value = parsed, .end = position};
}

struct ParsedChannel {
    double value;
    std::size_t end;
    bool percent;
};

[[nodiscard]] std::optional<ParsedChannel> parse_channel(
        std::string_view text, std::size_t position, bool allow_percent, bool allow_degrees) {
    auto number = parse_number(text, position);
    if (!number) return std::nullopt;
    position = number->end;
    bool percent = false;
    if (position < text.size() && text[position] == '%') {
        if (!allow_percent) return std::nullopt;
        percent = true;
        ++position;
    } else if (allow_degrees && position + 3 <= text.size() && lower_ascii(text[position]) == 'd' &&
               lower_ascii(text[position + 1]) == 'e' && lower_ascii(text[position + 2]) == 'g') {
        position += 3;
    }
    return ParsedChannel{.value = number->value, .end = position, .percent = percent};
}

[[nodiscard]] std::optional<std::array<ParsedChannel, 3>> parse_channels(std::string_view value,
        std::string_view function,
        bool first_percent,
        bool second_percent,
        bool third_percent,
        bool first_degrees,
        bool third_degrees) {
    if (value.size() < function.size() + 2) return std::nullopt;
    for (std::size_t index = 0; index < function.size(); ++index) {
        if (lower_ascii(value[index]) != function[index]) return std::nullopt;
    }
    auto position = function.size();
    if (value[position] != '(') return std::nullopt;
    ++position;
    const auto skip_space = [&]() {
        const auto start = position;
        while (position < value.size() && is_space(value[position]))
            ++position;
        return position > start;
    };
    skip_space();
    const auto first = parse_channel(value, position, first_percent, first_degrees);
    if (!first) return std::nullopt;
    position = first->end;
    if (!skip_space()) return std::nullopt;
    const auto second = parse_channel(value, position, second_percent, false);
    if (!second) return std::nullopt;
    position = second->end;
    if (!skip_space()) return std::nullopt;
    const auto third = parse_channel(value, position, third_percent, third_degrees);
    if (!third) return std::nullopt;
    position = third->end;
    skip_space();
    if (position >= value.size() || value[position] != ')' || position + 1 != value.size()) return std::nullopt;
    return std::array<ParsedChannel, 3>{*first, *second, *third};
}

[[nodiscard]] double channel_value(const ParsedChannel& channel) { return channel.value / (channel.percent ? 100 : 1); }

[[nodiscard]] RgbColor indexed_to_rgb(std::uint8_t index) {
    if (index < 16) return kBasicColors[index];
    if (index < 232) {
        const auto cube = static_cast<int>(index) - 16;
        return {.red = kColorCubeValues[static_cast<std::size_t>(cube / 36)],
                .green = kColorCubeValues[static_cast<std::size_t>((cube % 36) / 6)],
                .blue = kColorCubeValues[static_cast<std::size_t>(cube % 6)]};
    }
    const auto gray = kGrayValues[static_cast<std::size_t>(index) - 232];
    return {.red = gray, .green = gray, .blue = gray};
}

} // namespace

support::Expected<OklchColorValue> oklch_color(double lightness, double chroma, double hue) {
    if (!std::isfinite(lightness) || lightness < 0 || lightness > 1) {
        return std::unexpected(invalid_color(std::format("l must be between 0 and 1: {}", lightness)));
    }
    if (!std::isfinite(chroma) || chroma < 0) {
        return std::unexpected(invalid_color(std::format("c must not be negative: {}", chroma)));
    }
    if (!std::isfinite(hue)) return std::unexpected(invalid_color(std::format("h must be finite: {}", hue)));
    return OklchColorValue{.l = lightness, .c = chroma, .h = normalize_hue(hue)};
}

support::Expected<RgbColor> okhsl_to_rgb(double hue, double saturation, double lightness) {
    if (!std::isfinite(hue)) return std::unexpected(invalid_color(std::format("h must be finite: {}", hue)));
    if (!std::isfinite(saturation) || saturation < 0 || saturation > 1) {
        return std::unexpected(invalid_color(std::format("s must be between 0 and 1: {}", saturation)));
    }
    if (!std::isfinite(lightness) || lightness < 0 || lightness > 1) {
        return std::unexpected(invalid_color(std::format("l must be between 0 and 1: {}", lightness)));
    }
    return okhsl_to_rgb_unchecked(hue, saturation, lightness);
}

support::Expected<Color> parse_color(int index) {
    auto indexed = indexed_color(index);
    if (!indexed) return std::unexpected(indexed.error());
    return Color{*indexed};
}

support::Expected<Color> parse_color(double index) {
    if (!std::isfinite(index) || std::floor(index) != index || index < 0 || index > 255) {
        return std::unexpected(
                invalid_color(std::format("ANSI color index must be an integer from 0 to 255: {}", index)));
    }
    return parse_color(static_cast<int>(index));
}

support::Expected<Color> parse_color(std::string_view value) {
    const auto invalid = [&]() -> support::Expected<Color> {
        return std::unexpected(invalid_color("Invalid color value: " + std::string(value)));
    };
    if (value.size() == 4 || value.size() == 7) {
        if (!value.starts_with('#')) return invalid();
        if (value.size() == 4) {
            const auto red = parse_hex_byte(value.substr(1, 1));
            const auto green = parse_hex_byte(value.substr(2, 1));
            const auto blue = parse_hex_byte(value.substr(3, 1));
            if (!red || !green || !blue) return invalid();
            return Color{RgbColor{.red = static_cast<double>(*red * 17),
                    .green = static_cast<double>(*green * 17),
                    .blue = static_cast<double>(*blue * 17)}};
        }
        const auto red = parse_hex_byte(value.substr(1, 2));
        const auto green = parse_hex_byte(value.substr(3, 2));
        const auto blue = parse_hex_byte(value.substr(5, 2));
        if (!red || !green || !blue) return invalid();
        return Color{RgbColor{.red = static_cast<double>(*red),
                .green = static_cast<double>(*green),
                .blue = static_cast<double>(*blue)}};
    }

    if (const auto channels = parse_channels(value, "oklch", true, false, false, false, true)) {
        auto color = oklch_color(channel_value((*channels)[0]), channel_value((*channels)[1]), (*channels)[2].value);
        if (!color) return std::unexpected(color.error());
        return Color{*color};
    }
    if (const auto channels = parse_channels(value, "okhsl", false, true, true, true, false)) {
        auto rgb = okhsl_to_rgb((*channels)[0].value, channel_value((*channels)[1]), channel_value((*channels)[2]));
        if (!rgb) return std::unexpected(rgb.error());
        return Color{*rgb};
    }
    return invalid();
}

support::Expected<RgbColor> color_to_rgb(const Color& color) {
    if (const auto* rgb = std::get_if<RgbColor>(&color)) {
        return rgb_color(rgb->red, rgb->green, rgb->blue);
    }
    if (const auto* indexed = std::get_if<IndexedColor>(&color)) return indexed_to_rgb(indexed->index);
    const auto& oklch = std::get<OklchColorValue>(color);
    auto checked = oklch_color(oklch.l, oklch.c, oklch.h);
    if (!checked) return std::unexpected(checked.error());
    return oklch_to_rgb(*checked);
}

support::Expected<OklchColorValue> color_to_oklch(const Color& color) {
    if (const auto* oklch = std::get_if<OklchColorValue>(&color)) return oklch_color(oklch->l, oklch->c, oklch->h);
    auto rgb = color_to_rgb(color);
    if (!rgb) return std::unexpected(rgb.error());
    const auto lab = rgb_to_oklab(*rgb);
    return OklchColorValue{.l = lab.x,
            .c = std::hypot(lab.y, lab.z),
            .h = normalize_hue(std::atan2(lab.z, lab.y) * 180 / std::numbers::pi)};
}

support::Expected<OkhslChannels> color_to_okhsl(const Color& color) {
    auto rgb = color_to_rgb(color);
    if (!rgb) return std::unexpected(rgb.error());
    return rgb_to_okhsl_unchecked(*rgb);
}

support::Expected<std::string> color_to_hex(const Color& color) {
    auto rgb = color_to_rgb(color);
    if (!rgb) return std::unexpected(rgb.error());
    return std::format("#{:02x}{:02x}{:02x}",
            static_cast<int>(std::round(rgb->red)),
            static_cast<int>(std::round(rgb->green)),
            static_cast<int>(std::round(rgb->blue)));
}

support::Expected<Color> mix_colors(const Color& first, const Color& second, double amount, ColorMixSpace space) {
    if (!std::isfinite(amount) || amount < 0 || amount > 1) {
        return std::unexpected(invalid_color(std::format("amount must be between 0 and 1: {}", amount)));
    }
    if (space == ColorMixSpace::Srgb) {
        auto a = color_to_rgb(first);
        if (!a) return std::unexpected(a.error());
        auto b = color_to_rgb(second);
        if (!b) return std::unexpected(b.error());
        return Color{RgbColor{
                .red = a->red + (b->red - a->red) * amount,
                .green = a->green + (b->green - a->green) * amount,
                .blue = a->blue + (b->blue - a->blue) * amount,
        }};
    }

    auto a = color_to_oklch(first);
    if (!a) return std::unexpected(a.error());
    auto b = color_to_oklch(second);
    if (!b) return std::unexpected(b.error());
    const auto first_hue = a->c < 1e-7 ? b->h : a->h;
    const auto second_hue = b->c < 1e-7 ? first_hue : b->h;
    const auto hue_delta = std::fmod(std::fmod(second_hue - first_hue + 540, 360.0) + 360, 360.0) - 180;
    auto mixed =
            oklch_color(a->l + (b->l - a->l) * amount, a->c + (b->c - a->c) * amount, first_hue + hue_delta * amount);
    if (!mixed) return std::unexpected(mixed.error());
    return Color{*mixed};
}

} // namespace cch::tui
