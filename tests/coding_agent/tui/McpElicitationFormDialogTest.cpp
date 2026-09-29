// The form-mode Pending Elicitation dialog (issue #846; spec #833 story 26).
//
// The dialog is the Native TUI half of a Multi Round-Trip suspension, and it
// is reached only through the session's `cch_coding_agent` projection: this
// file includes no `cch/mcp` header and cannot, which is the headless-no-
// frontend invariant (ADR 0065) asserted by construction.
//
// The claims under test are the ones a user would notice, and the one a
// server would notice:
//
//   * the fields the Upstream's JSON Schema declares are on screen, each with
//     its own constraint, so the question is answerable;
//   * typing goes into the focused field, one `Input` and a focus index, and
//     a `d` is a `d` rather than a control;
//   * **invalid input is rejected in-dialog and never sent**: a rejected value
//     produces an inline error under its own field, the dialog stays up, and
//     no answer leaves it;
//   * a value is typed on the way out, so a field declared `number` answers
//     as a JSON number and not as a quoted string;
//   * accept, decline, and cancel are three distinct answers, and only accept
//     carries values;
//   * an untrusted schema cannot break the layout or the frame: the field
//     count, the label width, the nesting depth, and the schema's size are
//     all bounded, and a form the build will not read says so on screen
//     instead of drawing what it cannot carry;
//   * every line fits the render width, at the narrow widths the composed
//     render path enforces.

#include "coding_agent/tui/McpElicitationForm.hpp"
#include "coding_agent/tui/McpElicitationFormDialog.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/OverlayWidthBound.hpp"

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;
using coding_agent::McpElicitationAction;
using coding_agent::McpElicitationFormValues;
using coding_agent::McpElicitationMode;
using coding_agent::McpPendingElicitation;
using coding_agent::tui::McpElicitationFormDialog;
using coding_agent::tui::McpFormSchemaFault;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

/// A form with a required whole number, a boolean, and a required text field
/// with a length constraint: the three types the dialog edits, and enough
/// fields to make the focus index observable.
///
/// The properties are written in an order that is **not** the order the
/// dialog renders them, on purpose. A JSON object has no order, this Owner
/// reads it as a value tree, and the fields therefore render in that tree's
/// own key order. Pinning that here means a case that assumes a different
/// order is a case about the wrong thing.
constexpr std::string_view kThreeFieldSchema = R"({
  "type": "object",
  "title": "Confirm the deployment",
  "description": "The Upstream will not run the tool until you answer.",
  "properties": {
    "target": {
      "type": "string",
      "title": "Target environment",
      "description": "Where the change goes.",
      "minLength": 3
    },
    "replicas": {"type": "integer", "title": "Replicas", "minimum": 1, "maximum": 9},
    "notify": {"type": "boolean", "title": "Notify the on-call"}
  },
  "required": ["target", "replicas"]
})";

/// The rendered index of the field with this property name in
/// `kThreeFieldSchema`, which is the value tree's key order rather than the
/// order the schema wrote them in.
[[nodiscard]] std::size_t field_index(std::string_view name) {
    const auto form = coding_agent::tui::read_form_schema(kThreeFieldSchema);
    for (std::size_t index = 0; index < form.fields.size(); ++index) {
        if (form.fields[index].name == name) {
            return index;
        }
    }
    return form.fields.size();
}

[[nodiscard]] McpPendingElicitation pending(std::string schema) {
    return McpPendingElicitation{
            .elicitation_id = "executor#1",
            .server_id = "executor",
            .tool_name = "deploy",
            .mode = McpElicitationMode::Form,
            .request_id = "r1",
            .message = "Confirm before the deployment runs",
            .form_schema = std::move(schema),
    };
}

/// One dialog plus the answers it produced, so a case asserts on what the
/// user did rather than on the dialog's internals.
struct FormFixture {
    std::vector<std::tuple<McpElicitationAction, std::string, McpElicitationFormValues>> answers{};
    int invalidations{0};
    /// The dialog borrows the palette and reads the keybinding registry, the
    /// way the host's theme controller outlives every flow it owns, so both
    /// live here rather than in a temporary.
    coding_agent::tui::LiveTheme theme{test_theme()};
    std::shared_ptr<const tui::KeybindingRegistry> keybindings{test_keybindings()};
    std::shared_ptr<McpElicitationFormDialog> dialog{};

    explicit FormFixture(std::string schema = std::string{kThreeFieldSchema}) {
        dialog = std::make_shared<McpElicitationFormDialog>(
                theme,
                keybindings,
                pending(std::move(schema)),
                [this](McpElicitationAction action, std::string id, McpElicitationFormValues values) {
                    answers.emplace_back(action, std::move(id), std::move(values));
                },
                [this] { ++invalidations; });
        dialog->set_focused(true);
    }

    void press(std::string key) {
        static_cast<void>(dialog->handle_input(tui::KeyEvent{.key = std::move(key)}));
    }

    void press_shift(std::string key) {
        static_cast<void>(dialog->handle_input(tui::KeyEvent{.key = std::move(key), .shift = true}));
    }

    void decline() { static_cast<void>(dialog->handle_input(tui::KeyEvent{.key = "d", .alt = true})); }

    void type(std::string_view text) {
        for (const char character : text) {
            press(std::string(1, character));
        }
    }

    [[nodiscard]] std::string rendered(std::size_t width) const {
        auto result = dialog->render(width);
        REQUIRE(result.has_value());
        std::string text;
        for (const auto& line : result->lines) {
            text += tests::strip_ansi(line);
            text += '\n';
        }
        return text;
    }
};

} // namespace

TEST_CASE("the form dialog shows the fields the schema declares",
        "[coding_agent][tui][mcp][issue846][spec]") {
    FormFixture fixture;
    // The fields render in the value tree's key order, which for this schema
    // is not the order the schema wrote them in.
    CHECK(field_index("notify") == 0);
    CHECK(field_index("replicas") == 1);
    CHECK(field_index("target") == 2);
    const auto screen = fixture.rendered(80);
    // The Upstream is named, exactly as the URL dialog names it: the user is
    // answering one upstream's question, not the session's.
    CHECK(screen.find("executor") != std::string::npos);
    CHECK(screen.find("deploy") != std::string::npos);
    CHECK(screen.find("Confirm before the deployment runs") != std::string::npos);
    // The schema's own title and description head the form.
    CHECK(screen.find("Confirm the deployment") != std::string::npos);
    CHECK(screen.find("will not run the tool until you answer") != std::string::npos);
    // Every field, with the label the schema gave it and the constraint that
    // decides whether the answer is valid.
    CHECK(screen.find("Target environment") != std::string::npos);
    CHECK(screen.find("Where the change goes.") != std::string::npos);
    CHECK(screen.find("required") != std::string::npos);
    CHECK(screen.find("Replicas") != std::string::npos);
    CHECK(screen.find("Notify the on-call") != std::string::npos);
    CHECK(screen.find("true or false") != std::string::npos);
    // And the three answers are named on screen, the way the URL dialog names
    // them, so a control is not left to a keybinding the user must know.
    CHECK(screen.find("done") != std::string::npos);
    CHECK(screen.find("decline") != std::string::npos);
    CHECK(screen.find("cancel") != std::string::npos);
    CHECK(screen.find("next field") != std::string::npos);
}

TEST_CASE("typing goes into the focused field and the focus index moves between them",
        "[coding_agent][tui][mcp][issue846][spec]") {
    FormFixture fixture;
    REQUIRE(fixture.dialog->focused_field() == 0);
    fixture.type("prod");
    CHECK(fixture.dialog->field_value(0) == "prod");
    // A `d` is a `d`, not the decline control: the dialog's controls are
    // `alt+d` and the modifier is what tells them apart.
    fixture.type("d");
    CHECK(fixture.dialog->field_value(0) == "prodd");
    CHECK(fixture.answers.empty());
    // Tab moves to the next field, and the first field keeps its text.
    fixture.press("tab");
    REQUIRE(fixture.dialog->focused_field() == 1);
    CHECK(fixture.dialog->field_value(0) == "prodd");
    fixture.type("3");
    // Down moves the same way and shift+tab moves back, with nothing lost.
    fixture.press("down");
    REQUIRE(fixture.dialog->focused_field() == 2);
    fixture.press_shift("tab");
    REQUIRE(fixture.dialog->focused_field() == 1);
    CHECK(fixture.dialog->field_value(1) == "3");
    fixture.press("up");
    REQUIRE(fixture.dialog->focused_field() == 0);
    CHECK(fixture.dialog->focused_field() == 0);
    CHECK(fixture.dialog->field_value(0) == "prodd");
    // And the focus wraps, so no field is a dead end.
    fixture.press_shift("tab");
    REQUIRE(fixture.dialog->focused_field() == 2);
}

TEST_CASE("invalid input is rejected in the dialog and never sent",
        "[coding_agent][tui][mcp][issue846][spec]") {
    FormFixture fixture;
    const auto target = field_index("target");
    fixture.press("tab");
    fixture.press("tab");
    REQUIRE(fixture.dialog->focused_field() == target);
    fixture.type("ab"); // below minLength
    fixture.press("enter");
    // Nothing left the dialog: no answer at all, not an answer with bad data.
    CHECK(fixture.answers.empty());
    CHECK(fixture.dialog->live());
    // The error names the field and the constraint, and it is on screen under
    // that field rather than in a transient status line.
    const auto screen = fixture.rendered(80);
    INFO(screen);
    CHECK(screen.find("Target environment must be at least 3 characters") != std::string::npos);
    // The other required field is empty, and that is an error too.
    CHECK(fixture.dialog->field_error(field_index("replicas")) == "Replicas is required");
    // Focus moved to the first offending field, which is the empty required
    // one rather than the short one, so the next keypress lands where the
    // dialog is pointing.
    const auto replicas = field_index("replicas");
    REQUIRE(fixture.dialog->focused_field() == replicas);
    fixture.type("3");
    CHECK(fixture.dialog->field_error(replicas).empty());
    // Typing into the target field clears that field's own error, and with
    // both fields satisfied the submit goes through.
    fixture.press("tab");
    REQUIRE(fixture.dialog->focused_field() == target);
    fixture.type("c");
    CHECK(fixture.dialog->field_error(target).empty());
    fixture.press("enter");
    REQUIRE(fixture.answers.size() == 1);
    CHECK(std::get<0>(fixture.answers.front()) == McpElicitationAction::Accept);
}

TEST_CASE("every schema constraint is enforced before an answer is produced",
        "[coding_agent][tui][mcp][issue846][spec]") {
    SECTION("a number that is not a number") {
        FormFixture fixture(R"({"type":"object","properties":{"n":{"type":"number"}},"required":["n"]})");
        fixture.type("12x");
        fixture.press("enter");
        CHECK(fixture.answers.empty());
        CHECK(fixture.dialog->field_error(0) == "n must be a number");
    }
    SECTION("a whole number that is not whole") {
        FormFixture fixture(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})");
        fixture.type("2.5");
        fixture.press("enter");
        CHECK(fixture.answers.empty());
        CHECK(fixture.dialog->field_error(0) == "n must be a whole number");
    }
    SECTION("a value below the minimum") {
        FormFixture fixture(R"({"type":"object","properties":{"n":{"type":"integer","minimum":2}}})");
        fixture.type("1");
        fixture.press("enter");
        CHECK(fixture.answers.empty());
        CHECK(fixture.dialog->field_error(0) == "n must be at least 2.000000");
    }
    SECTION("a value above the maximum") {
        FormFixture fixture(R"({"type":"object","properties":{"n":{"type":"integer","maximum":2}}})");
        fixture.type("3");
        fixture.press("enter");
        CHECK(fixture.answers.empty());
        CHECK(fixture.dialog->field_error(0) == "n must be at most 2.000000");
    }
    SECTION("a value past maxLength") {
        FormFixture fixture(R"({"type":"object","properties":{"s":{"type":"string","maxLength":2}}})");
        fixture.type("abc");
        fixture.press("enter");
        CHECK(fixture.answers.empty());
        CHECK(fixture.dialog->field_error(0) == "s must be at most 2 characters");
    }
    SECTION("a value outside the enum") {
        FormFixture fixture(R"({"type":"object","properties":{"e":{"type":"string","enum":["a","b"]}}})");
        fixture.type("c");
        fixture.press("enter");
        CHECK(fixture.answers.empty());
        CHECK(fixture.dialog->field_error(0) == "e must be one of: a, b");
        // The choices are on screen before the user fails, so the constraint
        // is discoverable rather than only discoverable by being wrong.
        CHECK(fixture.rendered(80).find("one of: a, b") != std::string::npos);
        fixture.press("backspace");
        fixture.press("backspace");
        fixture.press("enter");
        REQUIRE(fixture.answers.size() == 1);
    }
    SECTION("a boolean that is neither true nor false") {
        FormFixture fixture(R"({"type":"object","properties":{"b":{"type":"boolean"}}})");
        fixture.type("yes");
        fixture.press("enter");
        CHECK(fixture.answers.empty());
        CHECK(fixture.dialog->field_error(0) == "b must be true or false");
    }
    SECTION("an optional field left empty is omitted, not sent as an empty string") {
        FormFixture fixture(R"({"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}}})");
        fixture.type("only-a");
        fixture.press("enter");
        REQUIRE(fixture.answers.size() == 1);
        const auto& values = std::get<2>(fixture.answers.front());
        REQUIRE(values.size() == 1);
        CHECK(values.at("a").get<std::string>() == "only-a");
    }
}

TEST_CASE("form values are typed on the way out", "[coding_agent][tui][mcp][issue846][spec]") {
    FormFixture fixture;
    fixture.press("tab");
    fixture.press("tab");
    REQUIRE(fixture.dialog->focused_field() == field_index("target"));
    fixture.type("prod");
    fixture.press("tab");
    REQUIRE(fixture.dialog->focused_field() == field_index("notify"));
    fixture.type("true");
    fixture.press("tab");
    REQUIRE(fixture.dialog->focused_field() == field_index("replicas"));
    fixture.type("3");
    fixture.press("enter");
    REQUIRE(fixture.answers.size() == 1);
    const auto& values = std::get<2>(fixture.answers.front());
    REQUIRE(values.size() == 3);
    // A field the schema declared as text answers as a JSON string...
    CHECK(values.at("target").get<std::string>() == "prod");
    // ...and one it declared as an integer answers as a number, not as the
    // quoted text the user typed. A server that declared a number would
    // otherwise have to guess whether "3" was three.
    CHECK(values.at("replicas").get<double>() == 3.0);
    CHECK(values.at("notify").get<bool>());
}

TEST_CASE("decline and cancel answer the question and carry no values",
        "[coding_agent][tui][mcp][issue846][spec]") {
    SECTION("decline") {
        FormFixture fixture;
        fixture.press("tab");
        fixture.press("tab");
        fixture.type("prod");
        fixture.press("tab");
        fixture.type("true");
        fixture.press("tab");
        fixture.type("3");
        // Nothing is validated on the way out of a refusal, so a form that
        // could not have been submitted still declines.
        fixture.decline();
        REQUIRE(fixture.answers.size() == 1);
        CHECK(std::get<0>(fixture.answers.front()) == McpElicitationAction::Decline);
        CHECK(std::get<1>(fixture.answers.front()) == "executor#1");
        // The user refused, so what they had typed does not travel with it.
        CHECK(std::get<2>(fixture.answers.front()).empty());
        CHECK_FALSE(fixture.dialog->live());
    }
    SECTION("cancel") {
        FormFixture fixture;
        fixture.press("tab");
        fixture.press("tab");
        fixture.type("prod");
        fixture.press("escape");
        REQUIRE(fixture.answers.size() == 1);
        CHECK(std::get<0>(fixture.answers.front()) == McpElicitationAction::Cancel);
        CHECK(std::get<2>(fixture.answers.front()).empty());
        CHECK_FALSE(fixture.dialog->live());
    }
    SECTION("the first settlement wins") {
        FormFixture fixture;
        fixture.press("tab");
        fixture.press("tab");
        fixture.type("prod");
        fixture.press("tab");
        fixture.type("true");
        fixture.press("tab");
        fixture.type("3");
        fixture.press("enter");
        fixture.decline();
        fixture.press("escape");
        REQUIRE(fixture.answers.size() == 1);
        CHECK(std::get<0>(fixture.answers.front()) == McpElicitationAction::Accept);
    }
    SECTION("a withdrawn dialog answers nothing") {
        // Session Close withdraws the dialog: the call behind it is being torn
        // down, so there is nothing left to answer.
        FormFixture fixture;
        fixture.press("tab");
        fixture.press("tab");
        fixture.type("prod");
        fixture.dialog->withdraw();
        CHECK_FALSE(fixture.dialog->live());
        CHECK(fixture.answers.empty());
        fixture.press("enter");
        fixture.decline();
        fixture.press("escape");
        CHECK(fixture.answers.empty());
    }
}

TEST_CASE("a schema this build will not read is reported on screen and answers nothing",
        "[coding_agent][tui][mcp][issue846][spec]") {
    SECTION("not JSON at all") {
        FormFixture fixture("this is not a schema");
        const auto screen = fixture.rendered(80);
        CHECK(screen.find("not readable JSON") != std::string::npos);
        CHECK(fixture.dialog->focused_field() == std::nullopt);
        // There is nothing to fill in, so the answers are still reachable:
        // a form the build cannot read is still a question with a decline.
        fixture.press("enter");
        REQUIRE(fixture.answers.size() == 1);
        CHECK(std::get<0>(fixture.answers.front()) == McpElicitationAction::Accept);
        CHECK(std::get<2>(fixture.answers.front()).empty());
    }
    SECTION("not an object") {
        FormFixture fixture("[]");
        const auto decoded = coding_agent::tui::read_form_schema("[]");
        CHECK(decoded.fault == McpFormSchemaFault::NotAnObject);
        const auto screen = fixture.rendered(80);
        INFO(screen);
        CHECK(screen.find("not a JSON object") != std::string::npos);
    }
    SECTION("nested past the depth bound") {
        // A schema nested far deeper than a stack can hold is a crash, not a
        // form. The bound is what keeps the reader away from one, and the
        // notice is what tells the user the answer will carry no fields.
        std::string deep = R"({"type":"object","properties":{"a":)";
        for (int depth = 0; depth < 400; ++depth) {
            deep += R"({"x":)";
        }
        deep += "1";
        for (int depth = 0; depth < 400; ++depth) {
            deep += "}";
        }
        deep += "}}";
        const auto decoded = coding_agent::tui::read_form_schema(deep);
        CHECK(decoded.fault == McpFormSchemaFault::TooDeep);
        CHECK(decoded.fields.empty());
        FormFixture fixture(deep);
        CHECK(fixture.rendered(80).find("nested this deeply") != std::string::npos);
    }
    SECTION("past the size bound") {
        std::string huge = R"({"type":"object","properties":{"a":{"title":")";
        huge.append(coding_agent::tui::mcp_form_bound::kMaxSchemaBytes, 'x');
        huge += R"("}}})";
        const auto decoded = coding_agent::tui::read_form_schema(huge);
        CHECK(decoded.fault == McpFormSchemaFault::TooLarge);
    }
    SECTION("more fields than a dialog can carry") {
        std::string many = R"({"type":"object","properties":{)";
        for (std::size_t index = 0; index < 40; ++index) {
            if (index > 0) {
                many += ",";
            }
            many += R"("f)" + std::to_string(index) + R"(":{"type":"string"})";
        }
        many += "}}";
        const auto decoded = coding_agent::tui::read_form_schema(many);
        CHECK(decoded.fields.size() == coding_agent::tui::mcp_form_bound::kMaxFields);
        CHECK(decoded.declared == 40);
        FormFixture fixture(many);
        // The truncation is on screen: an answer silently missing twenty-four
        // fields is the one failure the user cannot see coming.
        const auto screen = fixture.rendered(80);
        INFO(screen);
        CHECK(screen.find("first 16 of 40 fields") != std::string::npos);
    }
    SECTION("a label too wide to draw") {
        FormFixture fixture(R"({"type":"object","properties":{"a":{"title":")" +
                            std::string(400, 'x') + R"("}}})");
        for (const auto width : tests::kNarrowOverlayWidths) {
            auto rendered = fixture.dialog->render(width);
            REQUIRE(rendered.has_value());
            INFO("bound " << width);
            tests::check_all_lines_bounded(*rendered, width);
        }
    }
}

TEST_CASE("a type this build has no editor for is still answered",
        "[coding_agent][tui][mcp][issue846][spec]") {
    // `array` has no editor here. Dropping the field would answer a question
    // the Upstream marked required with the field simply missing, so it is
    // asked as text and the value travels as the string the user typed.
    FormFixture fixture(R"({"type":"object","properties":{"tags":{"type":"array"}},"required":["tags"]})");
    fixture.type("alpha");
    fixture.press("enter");
    REQUIRE(fixture.answers.size() == 1);
    const auto& values = std::get<2>(fixture.answers.front());
    REQUIRE(values.size() == 1);
    CHECK(values.at("tags").get<std::string>() == "alpha");
}

TEST_CASE("a form with no fields at all is still a question with three answers",
        "[coding_agent][tui][mcp][issue846][spec]") {
    FormFixture fixture(R"({"type":"object"})");
    CHECK(fixture.dialog->focused_field() == std::nullopt);
    // No input row, so no cursor to report: a dialog with no field has
    // nowhere to put one.
    static_cast<void>(fixture.dialog->render(80));
    CHECK(fixture.dialog->cursor_location() == std::nullopt);
    fixture.decline();
    REQUIRE(fixture.answers.size() == 1);
    CHECK(std::get<0>(fixture.answers.front()) == McpElicitationAction::Decline);
}

TEST_CASE("the form dialog renders within the width it is handed",
        "[coding_agent][tui][mcp][issue846][spec]") {
    // A long Server Id, tool name, message, schema title, field label,
    // description, and enum: every line the dialog composes is bounded by the
    // text component, and the input row by the `Input`.
    std::string schema = R"({"type":"object","title":")" + std::string(200, 't') + R"(",)";
    schema += R"("description":")" + std::string(600, 'd') + R"(",)";
    schema += R"("properties":{"a-very-long-field-name-that-never-ends":{"type":"string",)";
    schema += R"("title":")" + std::string(300, 'n') + R"(","description":")" + std::string(400, 'e') + R"(",";
    schema += R"("enum":[)";
    for (std::size_t index = 0; index < 16; ++index) {
        if (index > 0) {
            schema += ",";
        }
        schema += R"(")" + std::string(70, 'e') + std::to_string(index) + R"(")";
    }
    schema += R"(]}},"required":["a-very-long-field-name-that-never-ends"]})";
    FormFixture fixture(schema);
    fixture.type(std::string(300, 'v'));
    for (const auto width : tests::kNarrowOverlayWidths) {
        auto rendered = fixture.dialog->render(width);
        REQUIRE(rendered.has_value());
        INFO("bound " << width);
        tests::check_all_lines_bounded(*rendered, width);
    }
}

TEST_CASE("the cursor follows the focused field", "[coding_agent][tui][mcp][issue846][spec]") {
    FormFixture fixture;
    static_cast<void>(fixture.dialog->render(80));
    const auto first = fixture.dialog->cursor_location();
    REQUIRE(first.has_value());
    // The input row is the one after the focused field's label, and moving
    // focus moves the cursor with it rather than leaving it in the first
    // field's row.
    fixture.press("tab");
    static_cast<void>(fixture.dialog->render(80));
    const auto second = fixture.dialog->cursor_location();
    REQUIRE(second.has_value());
    CHECK(second->row > first->row);
    // A dialog that does not have focus reports no cursor at all, so the host
    // does not park a caret in a window the user is not typing into.
    fixture.dialog->set_focused(false);
    static_cast<void>(fixture.dialog->render(80));
    CHECK(fixture.dialog->cursor_location() == std::nullopt);
}

TEST_CASE("the schema reader reports what it could not read",
        "[coding_agent][tui][mcp][issue846][spec]") {
    using coding_agent::tui::read_form_schema;
    SECTION("an object with no properties declares no fields") {
        const auto form = read_form_schema(R"({"type":"object"})");
        CHECK(form.fields.empty());
        CHECK(form.fault == McpFormSchemaFault::None);
        CHECK(form.notice() == std::nullopt);
    }
    SECTION("a title and description are carried, not interpreted") {
        const auto form = read_form_schema(R"({"title":"T","description":"D","properties":{}})");
        REQUIRE(form.title.has_value());
        CHECK(*form.title == "T");
        REQUIRE(form.description.has_value());
        CHECK(*form.description == "D");
    }
    SECTION("a malformed required entry marks no field") {
        const auto form = read_form_schema(R"({"properties":{"a":{}},"required":[7,"a"]})");
        REQUIRE(form.fields.size() == 1);
        CHECK(form.fields.front().required);
    }
    SECTION("an unparseable constraint validates nothing rather than everything") {
        const auto form = read_form_schema(
                R"({"properties":{"a":{"type":"string","minLength":"3","maximum":"x","enum":"nope"}}})");
        REQUIRE(form.fields.size() == 1);
        CHECK_FALSE(form.fields.front().min_length.has_value());
        CHECK_FALSE(form.fields.front().maximum.has_value());
        CHECK(form.fields.front().enum_values.empty());
    }
    SECTION("an empty property name cannot be an answer's key") {
        const auto form = read_form_schema(R"({"properties":{"":{"type":"string"},"a":{}}})");
        REQUIRE(form.fields.size() == 1);
        CHECK(form.fields.front().name == "a");
    }
    SECTION("the declared type is normalized, and an unknown one becomes text") {
        const auto form = read_form_schema(
                R"({"properties":{"a":{"type":"STRING"},"b":{"type":"Number"},"c":{"type":"widget"}}})");
        REQUIRE(form.fields.size() == 3);
        const auto type_of = [&form](const std::string& name) {
            for (const auto& field : form.fields) {
                if (field.name == name) {
                    return field.type;
                }
            }
            return std::string{};
        };
        CHECK(type_of("a") == "string");
        CHECK(type_of("b") == "number");
        CHECK(type_of("c") == "string");
    }
}
