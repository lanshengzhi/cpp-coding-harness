// Named pi-v1.0.4 acceptance for the reusable inline math capability (#973).
// Every expectation is read from the verified named bundle through
// `scripts/tui/evidence.py`; nothing here regenerates expectations from Pike or
// falls back to the historical `fixtures/pi-tui/*.json` root snapshots.

#include <cch/tui/Latex.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Utils.hpp>

#include "support/PiTuiEvidence.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

/// One frozen pi observation: `output` is std::nullopt where pi returned
/// `undefined`, which is the frozen failure value rather than a partial formula.
struct FrozenLatexObservation {
    std::string name;
    std::string source;
    std::optional<std::string> output;
};

[[nodiscard]] std::vector<FrozenLatexObservation> read_observations(const support::JsonValue& rows) {
    std::vector<FrozenLatexObservation> observations;
    for (const auto& row : rows.get_array()) {
        FrozenLatexObservation observation{
                .name = row.at("name").get_string(),
                .source = row.at("source").get_string(),
                .output = std::nullopt,
        };
        if (row.at("output").holds<std::string>()) observation.output = row.at("output").get_string();
        observations.push_back(std::move(observation));
    }
    return observations;
}

[[nodiscard]] const support::JsonValue* find_scenario(const support::JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return &scenario;
    }
    return nullptr;
}

[[nodiscard]] support::Expected<std::map<std::string, FrozenLatexObservation>> named_observations(
        std::string_view scenario, std::string_view group) {
    const auto bundle = tests::read_pi_tui_evidence("latex.json");
    if (!bundle) return std::unexpected(bundle.error());
    const auto* found = find_scenario(*bundle, scenario);
    if (found == nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "missing latex evidence scenario: " + std::string(scenario)));
    }
    std::map<std::string, FrozenLatexObservation> by_name;
    for (auto& observation : read_observations(found->at("expected").at(std::string(group)))) {
        by_name.emplace(observation.name, std::move(observation));
    }
    return by_name;
}

} // namespace

TEST_CASE("render_latex matches every frozen pi inline grammar observation", "[tui][latex][issue973][compat-pi]") {
    const auto observations = named_observations("inline-grammar", "inline");
    REQUIRE(observations);
    REQUIRE(observations->size() >= 40);
    for (const auto& [name, observation] : *observations) {
        CAPTURE(name);
        const auto rendered = tui::render_latex(observation.source);
        REQUIRE(observation.output);
        REQUIRE(rendered);
        // Compare the complete rendered value: a symbol-only or partial renderer
        // cannot reproduce group, fraction, root and script structure.
        CHECK(*rendered == *observation.output);
    }
}

TEST_CASE("render_latex reproduces the frozen frac one-half rendering", "[tui][latex][issue973][compat-pi]") {
    const auto observations = named_observations("inline-grammar", "inline");
    REQUIRE(observations);
    const auto& fraction = observations->at("frac-half");
    REQUIRE(fraction.source == "\\frac{1}{2}");
    REQUIRE(fraction.output == "1/2");
    // `1/2` distinguishes the fraction path from both a pass-through of the
    // source and a naive `\frac` -> `/` string replacement of nested groups.
    CHECK(tui::render_latex(fraction.source) == tui::render_latex("\\frac 1 2"));
    CHECK(*tui::render_latex("\\frac{\\frac{1}{2}}{3}") == "(1/2)/3");
}

TEST_CASE("render_latex reports the frozen failure value for unsupported and malformed source",
        "[tui][latex][issue973][compat-pi]") {
    const auto observations = named_observations("inline-failure-value", "failing");
    REQUIRE(observations);
    REQUIRE(observations->size() >= 10);
    for (const auto& [name, observation] : *observations) {
        CAPTURE(name);
        REQUIRE_FALSE(observation.output.has_value());
        // Dropping the source or returning a plausible partial formula fails
        // here; only the frozen failure value passes.
        CHECK_FALSE(tui::render_latex(observation.source).has_value());
    }
}

TEST_CASE("render_latex does not invent a rendering for a failure by keeping the source",
        "[tui][latex][issue973][compat-pi]") {
    const auto rendered = tui::render_latex("\\frac{1}");
    CHECK_FALSE(rendered.has_value());
    // A source-preserving fallback would echo the input; the failure value is
    // the absence of a value, not the input text.
    CHECK(tui::render_latex("x^").has_value() == false);
}

// Complete matrix and environment outputs below were checked against the frozen pi v1.0.4 latex.ts renderer.
TEST_CASE("render_latex aligns matrix columns and renders frozen delimiters", "[tui][latex][issue975][spec]") {
    const auto parenthesized = tui::render_latex(R"(\begin{pmatrix} a & long \\ xy & z \\ m & uvw \end{pmatrix})");
    REQUIRE(parenthesized);
    CHECK(*parenthesized == "⎛ a  │ long ⎞\n⎜ xy │ z    ⎟\n⎝ m  │ uvw  ⎠");
    CHECK(tui::visible_width("⎛ a  │ long ⎞") == 13);

    const auto bracketed = tui::render_latex(R"(\begin{bmatrix} 1 & 2 \\ 30 & 4 \\ 5 & 600 \end{bmatrix})");
    REQUIRE(bracketed);
    CHECK(*bracketed == "⎡ 1  │ 2   ⎤\n⎢ 30 │ 4   ⎥\n⎣ 5  │ 600 ⎦");

    const auto vertical = tui::render_latex(R"(\begin{vmatrix} a & abc \\ de & f \\ ghi & j \end{vmatrix})");
    REQUIRE(vertical);
    CHECK(*vertical == "│ a   │ abc │\n│ de  │ f   │\n│ ghi │ j   │");

    const auto braces = tui::render_latex(R"(\begin{Bmatrix} a & b \\ c & d \end{Bmatrix})");
    REQUIRE(braces);
    CHECK(*braces == "⎧ a │ b ⎫\n⎩ c │ d ⎭");

    const auto double_vertical = tui::render_latex(R"(\begin{Vmatrix} a & b \\ c & d \end{Vmatrix})");
    REQUIRE(double_vertical);
    CHECK(*double_vertical == "║ a │ b ║\n║ c │ d ║");

    const auto array = tui::render_latex(R"(\begin{array}{cc} a & long \\ xyz & b \end{array})");
    REQUIRE(array);
    CHECK(*array == "a   │ long\nxyz │ b   ");

    CHECK(tui::render_latex(R"(\begin{matrix} a & b \\ c & d \end{matrix})") == "a │ b\nc │ d");
    CHECK(tui::render_latex(R"(\begin{smallmatrix} a & b \\ c & d \end{smallmatrix})") == "a │ b\nc │ d");

    const auto embedded = tui::render_latex(R"(A=\begin{pmatrix} 1 & 2 \\ 3 & 4 \end{pmatrix} + B)");
    REQUIRE(embedded);
    CHECK(*embedded == "A = ⎛ 1 │ 2 ⎞ + B\n    ⎝ 3 │ 4 ⎠");

    const auto fraction_cell =
            tui::render_latex(R"(\begin{matrix} \frac{1}{2} & z \\ x & 3 \end{matrix})", {.display = true});
    REQUIRE(fraction_cell);
    CHECK(*fraction_cell == "1/2 │ z\nx   │ 3");
}

TEST_CASE("render_latex renders cases conditions and odd or even row layouts", "[tui][latex][issue975][spec]") {
    const auto odd = tui::render_latex(R"(\begin{cases} x, & if x>0 \\ -x, & x<0 \\ 0, & otherwise \end{cases})");
    REQUIRE(odd);
    CHECK(*odd == "⎧ x  if x > 0\n⎨ -x if x < 0\n⎩ 0  otherwise");
    CHECK(tui::visible_width("⎧ x  if x > 0") == 13);
    CHECK(tui::visible_width("⎨ -x if x < 0") == 13);

    const auto even =
            tui::render_latex(R"(\begin{cases} a, & if a \\ b, & when b \\ c, & c \\ d, & for d \end{cases})");
    REQUIRE(even);
    CHECK(*even == "⎧ a if a\n⎨ b when b\n⎨\n⎨ c if c\n⎩ d for d");
    CHECK(tui::visible_width("⎨ b when b") == 10);

    CHECK(tui::render_latex(R"(\begin{cases} a, & if a \end{cases})") == "⎧ a if a");
    CHECK(tui::render_latex(R"(\begin{cases*} a, & a \\ b, & b \end{cases*})") == "⎧ a if a\n⎨\n⎩ b if b");
}

TEST_CASE(
        "render_latex renders aligned and alignedat rows without cross-row alignment", "[tui][latex][issue975][spec]") {
    const auto aligned = tui::render_latex(R"(\begin{aligned} x &= y \\ a + b &= c \end{aligned})");
    REQUIRE(aligned);
    CHECK(*aligned == "x = y\na + b = c");

    const auto align = tui::render_latex(R"(\begin{align*} a &= b \\ c &= d \end{align*})");
    REQUIRE(align);
    CHECK(*align == "a = b\nc = d");

    const auto aligned_at =
            tui::render_latex(R"(\begin{alignedat}{2} x &= y & u &= v \\ aa &= bb & cc &= dd \end{alignedat})");
    REQUIRE(aligned_at);
    CHECK(*aligned_at == "x = y u = v\naa = bb cc = dd");

    for (const std::string_view environment :
            {"align", "gather", "gathered", "multline", "multline*", "split", "alignat", "alignat*"}) {
        const std::string arguments = environment == "alignat" || environment == "alignat*" ? "{1}" : "";
        const std::string source = "\\begin{" + std::string{environment} + "}" + arguments +
                                   " a &= b \\\\ c &= d \\end{" + std::string{environment} + "}";
        CAPTURE(environment);
        CHECK(tui::render_latex(source) == "a = b\nc = d");
    }
}

TEST_CASE("render_latex rejects unsupported and malformed environments", "[tui][latex][issue975][spec]") {
    CHECK_FALSE(tui::render_latex(R"(\begin{unknown} x \end{unknown})").has_value());
    CHECK_FALSE(tui::render_latex(R"(\begin{pmatrix} a & b \\ c & d)").has_value());
    CHECK_FALSE(tui::render_latex(R"(\begin{pmatrix} a & b \end{matrix})").has_value());
    CHECK_FALSE(tui::render_latex(R"(\begin{pmatrix} \frac{a & b \\ c & d \end{pmatrix})").has_value());
}

TEST_CASE(
        "render_latex is reusable outside Markdown and reports terminal widths", "[tui][latex][issue973][compat-pi]") {
    const auto rendered = tui::render_latex("E = mc^{2}");
    REQUIRE(rendered);
    CHECK(*rendered == "E = mc\xc2\xb2");
    // Widths come from the toolkit's own width seam, not from byte counts.
    CHECK(tui::visible_width(*rendered) == 7);
    CHECK(rendered->size() == 8);
    CHECK(tui::visible_width(*tui::render_latex("\\frac{a+b}{c-d}")) == 11);

    // A real consumer renders math through the public Component seam without
    // ever going through Markdown.
    constexpr std::size_t kWidth = 20;
    tui::Text text(*rendered, 0, 0);
    const auto frame = text.render(kWidth);
    REQUIRE(frame);
    REQUIRE(frame->lines.size() == 1);
    CHECK(frame->lines.front() == *rendered + std::string(kWidth - tui::visible_width(*rendered), ' '));
}

TEST_CASE("render_latex matches frozen display rows and baseline alignment", "[tui][latex][issue974][compat-pi]") {
    const auto observations = named_observations("display-option", "display");
    REQUIRE(observations);
    REQUIRE(observations->size() == 3);
    for (const auto& [name, observation] : *observations) {
        CAPTURE(name);
        REQUIRE(observation.output);
        const auto rendered = tui::render_latex(observation.source, {.display = true});
        REQUIRE(rendered);
        CHECK(*rendered == *observation.output);
    }

    const auto fraction = tui::render_latex("\\frac{1}{2}", {.display = true});
    REQUIRE(fraction);
    CHECK(*fraction == "1\n─\n2");
    CHECK(tui::visible_width("1") == 1);
    CHECK(tui::visible_width("─") == 1);
    CHECK(tui::visible_width("2") == 1);
    CHECK(tui::render_latex("\\frac{1}{2}") == "1/2");

    const auto nested = tui::render_latex("\\frac{\\frac{1}{2}}{3}", {.display = true});
    REQUIRE(nested);
    CHECK(*nested == "1/2\n───\n 3");
    CHECK(tui::visible_width("1/2") == 3);
    CHECK(tui::visible_width("───") == 3);
    CHECK(tui::visible_width(" 3") == 2);

    const auto limits = tui::render_latex("\\sum_{i=1}^{n}", {.display = true});
    REQUIRE(limits);
    CHECK(*limits == " n\n ∑\ni=1");
    CHECK(tui::render_latex("\\sum_{i=1}^{n}") == "∑ᵢ₌₁ⁿ");
}

// These additional layout rows were independently evaluated against pi's latex.ts at
// 7c10bd4337495ee613f2224843ecdf349b80d1df; complete rows distinguish baseline and padding behavior.
TEST_CASE("render_latex preserves nested display fraction root and script rows", "[tui][latex][issue974][compat-pi]") {
    const auto root_fraction = tui::render_latex("\\frac{\\sqrt{x^2+1}}{\\sqrt{y}}", {.display = true});
    REQUIRE(root_fraction);
    CHECK(*root_fraction == "√(x²+1)\n───────\n  √y");
    CHECK(tui::visible_width("√(x²+1)") == 7);
    CHECK(tui::visible_width("───────") == 7);
    CHECK(tui::visible_width("  √y") == 4);

    const auto scripted_fraction = tui::render_latex("\\frac{x^{AB}}{\\sqrt{y}}", {.display = true});
    REQUIRE(scripted_fraction);
    CHECK(*scripted_fraction == " AB\n x\n───\n√y");
    CHECK(tui::visible_width(" AB") == 3);
    CHECK(tui::visible_width(" x") == 2);
    CHECK(tui::visible_width("───") == 3);
    CHECK(tui::visible_width("√y") == 2);

    const auto aligned_text = tui::render_latex("x + \\frac{1}{2}", {.display = true});
    REQUIRE(aligned_text);
    CHECK(*aligned_text == "    1\nx + ─\n    2");
    CHECK(tui::visible_width("    1") == 5);
    CHECK(tui::visible_width("x + ─") == 5);
    CHECK(tui::visible_width("    2") == 5);

    const auto aligned_fractions = tui::render_latex("\\frac{1}{2} + \\frac{3}{4}", {.display = true});
    REQUIRE(aligned_fractions);
    CHECK(*aligned_fractions == "1   3\n─ + ─\n2   4");
    CHECK(tui::visible_width("1   3") == 5);
    CHECK(tui::visible_width("─ + ─") == 5);
    CHECK(tui::visible_width("2   4") == 5);
}

// Additional option observations were independently evaluated against pi's latex.ts at
// 7c10bd4337495ee613f2224843ecdf349b80d1df.
TEST_CASE("render_latex follows frozen display fraction and operator options", "[tui][latex][issue974][compat-pi]") {
    CHECK(tui::render_latex("\\frac{1}{2}", {.display = true}) == "1\n─\n2");
    CHECK(tui::render_latex("\\dfrac{1}{2}", {.display = true}) == "1\n─\n2");
    CHECK(tui::render_latex("\\tfrac{1}{2}", {.display = true}) == "1/2");

    CHECK(tui::render_latex("\\sum\\nolimits_{i=1}^{n}", {.display = true}) == "∑ᵢ₌₁ⁿ");
    const auto sum_limits = tui::render_latex("\\sum\\limits_{i=1}^{n}", {.display = true});
    REQUIRE(sum_limits);
    CHECK(*sum_limits == " n\n ∑\ni=1");
    CHECK(tui::visible_width(" n") == 2);
    CHECK(tui::visible_width(" ∑") == 2);
    CHECK(tui::visible_width("i=1") == 3);

    const auto lim_limits = tui::render_latex("\\lim_{n\\to\\infty}", {.display = true});
    REQUIRE(lim_limits);
    CHECK(*lim_limits == "lim\nn→∞");
    CHECK(tui::visible_width("lim") == 3);
    CHECK(tui::visible_width("n→∞") == 3);
    CHECK(tui::render_latex("\\lim_{n\\to\\infty}") == "lim[n→∞]");

    const auto named_limits = tui::render_latex("\\operatorname*{argmax}_{x} f(x)", {.display = true});
    REQUIRE(named_limits);
    CHECK(*named_limits == "argmax f(x)\n  x");
    CHECK(tui::visible_width("argmax f(x)") == 11);
    CHECK(tui::visible_width("  x") == 3);
}

TEST_CASE("render_latex reports frozen failures for malformed nested display syntax",
        "[tui][latex][issue974][compat-pi]") {
    CHECK_FALSE(tui::render_latex("\\frac{\\sqrt{x}}", {.display = true}).has_value());
    CHECK_FALSE(tui::render_latex("x^{\\frac{1}{2}", {.display = true}).has_value());
    CHECK_FALSE(tui::render_latex("\\sum_{i=1}}", {.display = true}).has_value());

    const auto inline_formula = tui::render_latex("E = mc^{2}");
    REQUIRE(inline_formula);
    CHECK(*inline_formula == "E = mc²");
    tui::Text text(*inline_formula, 0, 0);
    const auto frame = text.render(12);
    REQUIRE(frame);
    REQUIRE(frame->lines.size() == 1);
    CHECK(frame->lines.front() == *inline_formula + std::string(12 - tui::visible_width(*inline_formula), ' '));
}
