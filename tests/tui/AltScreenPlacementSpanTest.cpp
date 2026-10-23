// Named pi-v1.3.0 acceptance for the image placement-span repaint (#1035).
//
// The frozen trigger set has two tiers (pi `tui-alt-screen.ts:1703-1724`): a
// changed row at or around an image anchor repaints on every terminal, and —
// only under WezTerm with the Kitty protocol — a changed text row strictly
// inside a live placement span also repaints, re-placing the image AFTER every
// erase and text write (`drawKittyImagesLast`, `:1748-1751`). The observation
// is the COMPLETE per-phase write stream of a real frozen `TuiAltScreen` over
// pi's `VirtualTerminal`, captured by
// fixtures/pi-v1.3.0/audit-1035/capture-placement-span.mts into the
// unregistered `audit-1035` family (the serial merger applies the registry
// pin, so the expected bytes are read from the committed capture like
// OutputPadEvidenceTest, not through a registry read).
//
// The replay drives the highest public caller — a real `AltScreen` over a
// `VirtualTerminal` with a live Kitty placement — through three phases: the
// initial full redraw, a text change INSIDE the placement span (the
// discriminating case), and a text change OUTSIDE every span (the negative the
// trigger set must not fire on). Normalizations are the four the artifact
// declares, applied symmetrically to both streams: rename random Kitty image
// ids to first-appearance index, drop the frozen full-redraw `\x1b[2J` the C++
// fullscreen path does not emit, drop the per-frame hidden-cursor re-assert the
// C++ viewport writes only on change, and drop the frozen per-row
// `SEGMENT_RESET` decoration the C++ alt screen does not append.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Image.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/Json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;

/// One scoped process-environment variable: the WezTerm trigger keys off
/// `WEZTERM_PANE`/`TERM_PROGRAM` exactly like pi, so each scenario pins both
/// and restores the ambient values on destruction (owned environment state,
/// never leaked into the other scenario or the host).
class ScopedEnvironmentVariable final {
public:
    explicit ScopedEnvironmentVariable(std::string name) : name_(std::move(name)) {
        if (const auto* value = std::getenv(name_.c_str()); value != nullptr) original_ = value;
    }
    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    ~ScopedEnvironmentVariable() {
        if (original_)
            (void)::setenv(name_.c_str(), original_->c_str(), 1);
        else
            (void)::unsetenv(name_.c_str());
    }

    void set(std::string_view value) { (void)::setenv(name_.c_str(), std::string(value).c_str(), 1); }
    void unset() { (void)::unsetenv(name_.c_str()); }

private:
    std::string name_;
    std::optional<std::string> original_;
};

/// The frozen pi-v1.3.0 placement-span artifact. The bytes are the committed
/// capture; the C++ side never derives them.
[[nodiscard]] const JsonValue& placement_span_artifact() {
    static const auto artifact = [] {
        const std::string path = std::string{CCH_SOURCE_DIR} + "/fixtures/pi-v1.3.0/audit-1035/placement-span.json";
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input);
        const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        auto parsed = cch::support::read_json(text);
        REQUIRE(parsed.has_value());
        return *parsed;
    }();
    return artifact;
}

[[nodiscard]] const JsonValue& scenario_of(std::string_view name) {
    const auto& artifact = placement_span_artifact();
    for (const auto& scenario : artifact.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    FAIL("missing frozen placement-span scenario: " << std::string{name});
    return artifact;
}

void replace_all(std::string& text, std::string_view from, std::string_view to) {
    std::size_t cursor = 0;
    while ((cursor = text.find(from, cursor)) != std::string::npos) {
        text.replace(cursor, from.size(), to);
        cursor += to.size();
    }
}

/// Rename every Kitty image id in the stream to its first-appearance index, so
/// the two random id spaces compare by identity (pi `allocateImageId` is random
/// by construction). Only the control section of each `\x1b_G` command is
/// inspected — up to its payload separator `;` or, for payload-less commands
/// such as `a=p` and `d=a`, up to the `\x1b\\` terminator.
void rename_image_ids(std::string& text) {
    std::vector<std::pair<std::string, std::size_t>> seen;
    std::size_t command = 0;
    while ((command = text.find("\x1b_G", command)) != std::string::npos) {
        std::size_t cursor = command + 3;
        while (true) {
            // Recompute per iteration: the replacement below mutates `text`,
            // so bounds captured once could address stale offsets.
            const auto payload = text.find(';', command + 3);
            const auto terminator = text.find("\x1b\\", command + 3);
            const auto controls_end = payload == std::string::npos      ? terminator
                                      : terminator == std::string::npos ? payload
                                      : payload < terminator            ? payload
                                                                        : terminator;
            if (controls_end == std::string::npos) break;
            const auto at = text.find("i=", cursor);
            if (at == std::string::npos || at >= controls_end) break;
            std::size_t end = at + 2;
            while (end < controls_end && std::isdigit(static_cast<unsigned char>(text[end])))
                ++end;
            if (end == at + 2) {
                cursor = at + 2;
                continue;
            }
            const auto id = text.substr(at + 2, end - at - 2);
            const auto found =
                    std::find_if(seen.begin(), seen.end(), [&](const auto& entry) { return entry.first == id; });
            std::size_t number = 0;
            if (found == seen.end()) {
                number = seen.size();
                seen.emplace_back(id, number);
            } else {
                number = found->second;
            }
            const auto replacement = std::format("i=#{}", number);
            text.replace(at, id.size() + 2, replacement);
            cursor = at + replacement.size();
        }
        command = text.find("\x1b_G", cursor);
    }
}

/// The four artifact-declared normalizations, applied symmetrically so a byte
/// the C++ path never emits costs nothing on either side of the comparison.
[[nodiscard]] std::string normalized_stream(std::string text) {
    // The frozen full redraw appends "\x1b[2J" right after the image cleanup;
    // the C++ fullscreen path does not emit it (tracked outside this ticket).
    replace_all(text, "\x1b_Ga=d,d=A,q=2\x1b\\\x1b[2J", "\x1b_Ga=d,d=A,q=2\x1b\\");
    replace_all(text, "\x1b_Ga=d,d=a,q=2\x1b\\\x1b[2J", "\x1b_Ga=d,d=a,q=2\x1b\\");
    // Frozen re-asserts the hidden hardware cursor in every frame; the C++
    // viewport asserts cursor visibility only when it changes.
    replace_all(text, "\x1b[?25l", "");
    replace_all(text, "\x1b[?25h", "");
    // The frozen alt screen decorates every non-image row write with
    // SEGMENT_RESET; the C++ alt screen does not append it.
    replace_all(text, "\x1b[0m\x1b]8;;\x07", "");
    rename_image_ids(text);
    return text;
}

[[nodiscard]] std::string joined_writes(const cch::tui::VirtualTerminal& terminal, std::size_t from) {
    std::string joined;
    const auto& writes = terminal.output();
    for (std::size_t index = from; index < writes.size(); ++index)
        joined += writes[index];
    return joined;
}

[[nodiscard]] std::vector<std::string> string_lines(const JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

[[nodiscard]] std::vector<std::string> trimmed_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    rows.reserve(terminal.screen().size());
    for (const auto& row : terminal.screen()) {
        auto trimmed = row;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

/// The C++ shape of the frozen layout root: the rows it renders plus the
/// placement sidecar whose span covers rows [0, 3). The image stage owns the
/// anchor row; the covered text rows are the component's own lines.
class SpanChild final : public cch::tui::Component {
public:
    explicit SpanChild(std::vector<cch::tui::InlineImageRenderRegion> images) : images_(std::move(images)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_, .images = images_};
    }

    void invalidate() override {}
    void set_covered(std::string value) { lines_[1] = std::move(value); }
    void set_tail(std::string value) { lines_[3] = std::move(value); }

private:
    std::vector<std::string> lines_{"", "", "", "after"};
    std::vector<cch::tui::InlineImageRenderRegion> images_;
};

[[nodiscard]] cch::tui::InlineImageRenderRegion placement_region(const JsonValue& image) {
    cch::tui::InlineImageRenderRegion region;
    region.resource_id = static_cast<std::uint64_t>(image.at("id").get_number());
    region.revision = 1;
    region.encoded_data = image.at("data").get_string();
    region.mime_type = image.at("mimeType").get_string();
    region.pixel_width = static_cast<std::size_t>(image.at("widthPx").get_number());
    region.pixel_height = static_cast<std::size_t>(image.at("heightPx").get_number());
    region.fallback_text = cch::tui::image_fallback(
            region.mime_type, cch::tui::ImagePixelSize{.width = region.pixel_width, .height = region.pixel_height});
    region.region = cch::tui::CellRegion{.column = 0,
            .row = 0,
            .columns = static_cast<std::size_t>(image.at("columns").get_number()),
            .rows = static_cast<std::size_t>(image.at("rows").get_number())};
    region.source_rows = region.region.rows;
    region.rows_reserved_in_lines = true;
    return region;
}

/// Replays one frozen scenario phase by phase: the complete normalized write
/// stream and the terminal screen after each action must equal the frozen
/// observation byte for byte.
class PhaseChecker {
public:
    PhaseChecker(cch::tui::VirtualTerminal& terminal, const JsonValue& scenario)
        : terminal_(terminal), phases_(scenario.at("expected").at("phases").get_array()),
          from_(terminal.output().size()) {}

    void check(std::string_view action) {
        REQUIRE(phase_index_ < phases_.size());
        const auto& phase = phases_[phase_index_++];
        CHECK(phase.at("action").get_string() == action);
        std::string observed = normalized_stream(joined_writes(terminal_, from_));
        from_ = terminal_.output().size();
        std::string expected;
        for (const auto& write : phase.at("writes").get_array())
            expected += write.get_string();
        expected = normalized_stream(std::move(expected));
        CHECK(observed == expected);
        CHECK(trimmed_rows(terminal_) == string_lines(phase.at("viewport")));
    }

    void require_done() const { CHECK(phase_index_ == phases_.size()); }

private:
    cch::tui::VirtualTerminal& terminal_;
    const std::vector<JsonValue>& phases_;
    std::size_t phase_index_{0};
    std::size_t from_;
};

void replay_scenario(std::string_view name, bool wezterm) {
    const auto& scenario = scenario_of(name);
    const auto& image = scenario.at("inputs").at("image");

    ScopedEnvironmentVariable wezterm_pane("WEZTERM_PANE");
    ScopedEnvironmentVariable term_program("TERM_PROGRAM");
    term_program.unset();
    if (wezterm)
        wezterm_pane.set("1");
    else
        wezterm_pane.unset();

    cch::tui::VirtualTerminal terminal(
            {.columns = static_cast<std::size_t>(scenario.at("inputs").at("dimensions").at("columns").get_number()),
                    .rows = static_cast<std::size_t>(scenario.at("inputs").at("dimensions").at("rows").get_number()),
                    .capabilities = {.inline_images = cch::tui::InlineImageProtocol::Kitty}});
    cch::tui::AltScreen tui(terminal);
    std::vector<cch::tui::InlineImageRenderRegion> images;
    images.push_back(placement_region(image));
    auto child = std::make_unique<SpanChild>(std::move(images));
    auto* child_pointer = child.get();
    tui.set_layout_root(std::move(child));

    REQUIRE(tui.start());
    PhaseChecker check(terminal, scenario);
    REQUIRE(tui.render());
    check.check("initial");
    child_pointer->set_covered(scenario.at("inputs").at("content").at("changedCovered").get_string());
    REQUIRE(tui.render());
    check.check("covered-text-change");
    child_pointer->set_tail(scenario.at("inputs").at("content").at("changedTail").get_string());
    REQUIRE(tui.render());
    check.check("outside-span-change");
    check.require_done();
    REQUIRE(tui.stop());
}

/// A plain overlay body: the compositor paints it at the requested rect, and
/// the covered base row keeps its content beneath it.
class OverlayLine final : public cch::tui::Component {
public:
    explicit OverlayLine(std::string line) : line_(std::move(line)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {line_}};
    }

    void invalidate() override {}

private:
    std::string line_;
};

/// Replays the frozen overlay-inside-span scenario: an overlay row composed at
/// row 1 changes a row inside the live placement span, so the frozen WezTerm
/// trigger repaints the placement AFTER the overlay text, in both the shown
/// and the hidden frame.
void replay_overlay_scenario(std::string_view name) {
    const auto& scenario = scenario_of(name);
    const auto& inputs = scenario.at("inputs");
    const auto& image = inputs.at("image");
    const auto& overlay_inputs = inputs.at("content").at("overlay");

    ScopedEnvironmentVariable wezterm_pane("WEZTERM_PANE");
    ScopedEnvironmentVariable term_program("TERM_PROGRAM");
    term_program.unset();
    wezterm_pane.set("1");

    cch::tui::VirtualTerminal terminal(
            {.columns = static_cast<std::size_t>(inputs.at("dimensions").at("columns").get_number()),
                    .rows = static_cast<std::size_t>(inputs.at("dimensions").at("rows").get_number()),
                    .capabilities = {.inline_images = cch::tui::InlineImageProtocol::Kitty}});
    cch::tui::AltScreen tui(terminal);
    std::vector<cch::tui::InlineImageRenderRegion> images;
    images.push_back(placement_region(image));
    tui.set_layout_root(std::make_unique<SpanChild>(std::move(images)));

    REQUIRE(tui.start());
    PhaseChecker check(terminal, scenario);
    REQUIRE(tui.render());
    check.check("initial");

    auto overlay = std::make_unique<cch::tui::Overlay>(
            cch::tui::OverlayOptions{.width = cch::tui::SizeValue{overlay_inputs.at("width").get_number()},
                    .max_height = cch::tui::SizeValue{overlay_inputs.at("maxHeight").get_number()},
                    .row = cch::tui::SizeValue{overlay_inputs.at("row").get_number()},
                    .col = cch::tui::SizeValue{overlay_inputs.at("column").get_number()}});
    REQUIRE(overlay->add_child(std::make_unique<OverlayLine>(overlay_inputs.at("line").get_string())));
    auto* overlay_pointer = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.render());
    check.check("overlay-shown");

    REQUIRE(tui.remove_overlay(overlay_pointer));
    REQUIRE(tui.render());
    check.check("overlay-hidden");
    check.require_done();
    REQUIRE(tui.stop());
}

} // namespace

TEST_CASE("frozen WezTerm placement-span repaint emits image bytes after the erase and text passes",
        "[tui][image][alt-screen][issue1035][compat-pi]") {
    replay_scenario("wezterm-placement-span-repaint", true);
}

TEST_CASE("frozen non-WezTerm control rewrites the covered row without image commands",
        "[tui][image][alt-screen][issue1035][compat-pi]") {
    replay_scenario("plain-terminal-placement-span-control", false);
}

TEST_CASE("frozen WezTerm overlay inside a placement span repaints with image bytes last",
        "[tui][image][alt-screen][issue1035][compat-pi]") {
    replay_overlay_scenario("wezterm-overlay-inside-placement-span");
}
