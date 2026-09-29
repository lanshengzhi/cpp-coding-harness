#pragma once

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

/// The bounds an Upstream's form schema is held to before any of it reaches
/// the screen (issue #846; spec #833 story 26; ADR 0008). A schema is
/// **untrusted input**: it is text an Upstream MCP Server chose, and the
/// dialog renders whatever it says. These four values are the containment,
/// and `docs/runtime-capacities.md` records them with the Multi Round-Trip
/// bounds they sit beside.
namespace mcp_form_bound {
/// The largest schema this build will read. The wire already caps one
/// response at `kMaxResponseBytes`; this is the far smaller amount of text a
/// dialog can usefully draw, and a schema past it is not rendered at all.
inline constexpr std::size_t kMaxSchemaBytes{64 * 1024};
/// The deepest object/array nesting the schema may have before it is not
/// rendered. The JSON reader is recursive descent, so an adversarially nested
/// schema is a stack-overflow risk, and a stack overflow is a crash rather
/// than a form with no fields.
inline constexpr std::size_t kMaxNesting{32};
/// The most fields one form renders. A user cannot meaningfully fill in an
/// unbounded pile, and each field is a row of the dialog.
inline constexpr std::size_t kMaxFields{16};
/// The most visible columns of one schema-declared label, and of one field's
/// description, that reach the screen. A field named with a megabyte of text
/// is still one field; truncating the label keeps the row a row, and the
/// bound is in columns because that is the width the render path enforces.
inline constexpr std::size_t kMaxLabelColumns{80};
inline constexpr std::size_t kMaxDescriptionColumns{240};
/// The most `enum` values one field offers, for the same reason.
inline constexpr std::size_t kMaxEnumValues{16};
} // namespace mcp_form_bound

/// One field of a form-mode Pending Elicitation, as the dialog shows it
/// (issue #846; spec #833 story 26).
///
/// Values are entered as text in the single `cch::tui::Input` the dialog
/// owns and are **typed on the way out**: a field the schema declared
/// `number` answers as a JSON number. A type this build does not render is
/// carried as a string field, so a schema using one still gets an answer
/// rather than a silently dropped question.
struct McpElicitationField {
    /// The Upstream's own property name, and the key the answer is sent under.
    std::string name{};
    /// The label the dialog shows. The schema's `title`, else its `name`.
    std::string title{};
    /// The schema's `description`, truncated to the label bound.
    std::optional<std::string> description{};
    /// The declared JSON type, lower-cased: `string`, `number`, `integer`,
    /// `boolean`, or `array`/`object` when the schema asked for one this
    /// build has no editor for (carried as `string`).
    std::string type{"string"};
    bool required{false};
    /// The schema's `enum` values, for a string field. An `enum` on a
    /// non-string type is not honored and is not rendered.
    std::vector<std::string> enum_values{};
    std::optional<double> minimum{};
    std::optional<double> maximum{};
    std::optional<std::size_t> min_length{};
    std::optional<std::size_t> max_length{};
};

/// Why a schema was not rendered into fields. The dialog names the fault on
/// screen rather than showing an empty form as though the Upstream had asked
/// for nothing: a user who cannot see why a form is empty cannot answer it.
enum class McpFormSchemaFault {
    None,
    /// Past `kMaxSchemaBytes`.
    TooLarge,
    /// Deeper than `kMaxNesting`.
    TooDeep,
    /// Not parseable as JSON.
    Unreadable,
    /// Parseable, but not a JSON object, so it declares no fields.
    NotAnObject,
};

/// One form-mode elicitation's schema as the dialog reads it. The Upstream's
/// opaque continuation token is not here and never was: the schema is what
/// the user is asked about, and the token is the host's business.
struct McpElicitationForm {
    /// The schema's own `title`, shown above the fields.
    std::optional<std::string> title{};
    /// The schema's own `description`, shown under the title.
    std::optional<std::string> description{};
    /// The rendered fields, in the order this Owner reads the schema's
    /// properties in — which is the value tree's own key order, not the
    /// server's source order.
    std::vector<McpElicitationField> fields{};
    /// What is wrong with the schema, when something is.
    McpFormSchemaFault fault{McpFormSchemaFault::None};
    /// How many properties the schema declared, including any past the field
    /// bound. A total above `fields.size()` is a form that was truncated, and
    /// the user is told so instead of answering a partial question.
    std::size_t declared{0};

    /// Why the fields on screen are not every field the Upstream asked for:
    /// the fault that stopped the read, or a form cut at the field bound. The
    /// dialog shows this on screen, because a user who cannot see why a form
    /// is short cannot tell what their answer is missing.
    [[nodiscard]] std::optional<std::string> notice() const;
};

/// Read one Upstream form schema into the fields the dialog renders. Never
/// fails: a schema this build will not read produces zero fields and the
/// fault that says why, because a form the user cannot answer still has a
/// decline and a cancel, and a crash over a hostile schema is not a
/// disposition of anything.
[[nodiscard]] McpElicitationForm read_form_schema(std::string_view schema);

/// What one field's entered text means.
enum class McpFieldOutcome {
    /// The field was left empty and is not required, so the answer omits it.
    Absent,
    /// The text is a value the schema accepts.
    Value,
    /// The text does not satisfy the schema. The message is what the dialog
    /// shows under the field, and nothing is answered until it is fixed.
    Invalid,
};

struct McpFieldCoercion {
    McpFieldOutcome outcome{McpFieldOutcome::Absent};
    support::JsonValue value{};
    std::string error{};
};

/// Validate one field's text against the schema subset the dialog renders
/// and type it for the answer. Every constraint in `McpElicitationField` is
/// checked here, so a value that reaches the Upstream is one this function
/// accepted.
[[nodiscard]] McpFieldCoercion coerce_field(const McpElicitationField& field, std::string_view text);

} // namespace cch::coding_agent::tui
