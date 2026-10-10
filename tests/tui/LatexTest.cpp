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
    CHECK_FALSE(tui::render_latex("\\begin{matrix} a \\\\ b \\end{matrix}").has_value());
    CHECK_FALSE(tui::render_latex("\\begin{aligned} a &= b \\end{aligned}").has_value());
}

TEST_CASE("render_latex does not invent a rendering for a failure by keeping the source",
        "[tui][latex][issue973][compat-pi]") {
    const auto rendered = tui::render_latex("\\frac{1}");
    CHECK_FALSE(rendered.has_value());
    // A source-preserving fallback would echo the input; the failure value is
    // the absence of a value, not the input text.
    CHECK(tui::render_latex("x^").has_value() == false);
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

TEST_CASE("render_latex reports the frozen failure value for the deferred display layout",
        "[tui][latex][issue973][compat-pi]") {
    const auto bundle = tests::read_pi_tui_evidence("latex.json");
    REQUIRE(bundle);
    const auto* scenario = find_scenario(*bundle, "display-option");
    REQUIRE(scenario != nullptr);
    const auto& rows = scenario->at("expected").at("display").get_array();
    REQUIRE(rows.size() >= 3);
    for (const auto& row : rows) {
        CAPTURE(row.at("name").get_string());
        // The frozen pi display output is multi-line; #974 owns that layout.
        // This slice must report the failure value rather than the inline one.
        CHECK_FALSE(tui::render_latex(row.at("source").get_string(), {.display = true}).has_value());
    }
    const tui::LatexOptions display{.display = true};
    CHECK_FALSE(tui::render_latex("\\frac{1}{2}", display).has_value());
    CHECK(tui::render_latex("\\frac{1}{2}").has_value());
}

TEST_CASE("render_latex leaves the frozen pi display observation replayable for #974",
        "[tui][latex][issue973][compat-pi]") {
    const auto bundle = tests::read_pi_tui_evidence("latex.json");
    REQUIRE(bundle);
    const auto* scenario = find_scenario(*bundle, "display-option");
    REQUIRE(scenario != nullptr);
    const auto& fraction = scenario->at("expected").at("display").get_array().front();
    REQUIRE(fraction.at("output").get_string() == "1\n─\n2");
    CHECK(tui::visible_width(fraction.at("output").get_string()) == 3);
    CHECK(tui::render_latex(fraction.at("source").get_string(), {.display = true}) == std::nullopt);
}