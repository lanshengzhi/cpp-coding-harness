// Coverage for the session write-path redaction list (#666).
//
// `EntrySerializer`'s `redacted_message` redacts an `ai::MessageVariant` field
// by field before it reaches the pi v3 wire. That list is hand-written, so a
// field added to the AI message model without a matching redaction line used
// to leak silently: no compiler error, no failing test, no golden diff.
//
// This test is the missing alarm, and it is deliberately *not* a stand-in
// assertion about list names or lengths. It derives the write-side field set
// through Glaze's aggregate reflection, plants a distinct secret-shaped
// sentinel in every string-bearing leaf, and drives the real persistence entry
// point (`EntrySerializer::serialize_message`). A field that a redaction
// omission leaves unredacted is observed as a surviving sentinel.
//
// The set the redaction list must align with is the **write-side** field set:
// the `cch::ai` values the serializer is handed before `to_message_dto` turns
// them into the pi wire DTOs (redaction happens on the AI value, before
// conversion). The DTO's own field names are not what the list enumerates.

#include "agent/harness/session/EntrySerializer.hpp"
#include "agent/harness/session/SessionMessageJson.hpp"
#include "support/JsonGlaze.hpp"

#include <cch/ai/Message.hpp>
#include <cch/support/JsonValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

// ── reflection-derived sentinel injection ──

template <class T> struct is_optional : std::false_type {};
template <class U> struct is_optional<std::optional<U>> : std::true_type {};

template <class T> struct is_vector : std::false_type {};
template <class U> struct is_vector<std::vector<U>> : std::true_type {};

template <class T> struct is_variant : std::false_type {};
template <class... U> struct is_variant<std::variant<U...>> : std::true_type {};

struct PoisonedLeaf {
    std::string path;
    std::string sentinel;
};

struct PoisonRegistry {
    std::vector<PoisonedLeaf> leaves;
    /// A string-bearing leaf reached a type this walker does not know, so the
    /// coverage below would silently skip it. Failing on any entry forces the
    /// walker to learn the new shape instead of losing the field.
    std::vector<std::string> unhandled;
};

/// `sk-` plus at least eight key characters, so `support::redact_text`'s
/// prefixed-token rule always recognizes it. Path characters that are not key
/// characters are folded to `-` to keep the token contiguous, and a terminator
/// keeps a shorter leaf from matching inside a longer one (`text` inside
/// `text_signature`).
[[nodiscard]] std::string sentinel_for(std::string_view path) {
    std::string out{"sk-p-"};
    out.reserve(path.size() + 9);
    for (const char c : path) {
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '-');
    }
    out += "-probe";
    return out;
}

/// Plant a distinct sentinel in every string-bearing leaf reachable from
/// `value`, registering each leaf path. The leaf set is derived from the
/// declared shapes via `glz::reflect`, never hand-enumerated: a field added to
/// any AI message/content struct is injected automatically.
template <class T> void poison_leaves(T& value, const std::string& path, PoisonRegistry& registry) {
    using V = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<V, std::string>) {
        value = sentinel_for(path);
        registry.leaves.push_back(PoisonedLeaf{path, value});
    } else if constexpr (std::is_same_v<V, support::JsonValue>) {
        const auto sentinel = sentinel_for(path);
        value = support::JsonValue{support::JsonValue::object_t{{"probe", support::JsonValue{sentinel}}}};
        registry.leaves.push_back(PoisonedLeaf{path, sentinel});
    } else if constexpr (is_optional<V>::value) {
        value.emplace();
        poison_leaves(*value, path, registry);
    } else if constexpr (is_vector<V>::value) {
        using Element = typename V::value_type;
        value.clear();
        if constexpr (is_variant<Element>::value) {
            // One element per alternative, so every content alternative is
            // covered even though a variant value holds only one at a time.
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((value.emplace_back(std::in_place_index<I>), poison_leaves(value.back(), path + "[]", registry)), ...);
            }(std::make_index_sequence<std::variant_size_v<Element>>{});
        } else {
            value.emplace_back();
            poison_leaves(value.back(), path + "[]", registry);
        }
    } else if constexpr (is_variant<V>::value) {
        std::visit([&](auto& alternative) { poison_leaves(alternative, path, registry); }, value);
    } else if constexpr (std::is_aggregate_v<V> && requires { glz::reflect<V>::size; }) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((poison_leaves(
                     glz::get<I>(glz::to_tie(value)), path + "." + std::string(glz::reflect<V>::keys[I]), registry)),
                    ...);
        }(std::make_index_sequence<glz::reflect<V>::size>{});
    } else if constexpr (std::is_arithmetic_v<V> || std::is_enum_v<V>) {
        // Scalars carry no text.
    } else {
        registry.unhandled.push_back(path + " <" + typeid(V).name() + ">");
    }
}

// ── the reviewed exemption list ──

struct Exemption {
    std::string_view path;
    std::string_view authority;
};

/// Fields the write path deliberately leaves raw. Every entry states the
/// record that authorizes the exemption; a field may not be exempted "because
/// the test was red" (#666). `BashExecutionMessage.output` is exempt: ADR 0028's
/// Output section records "No redaction" for User Bash output, #677 chose
/// option 甲 on that basis, and #679 removed the redaction this test used to
/// require.
constexpr std::array kExemptions{
        Exemption{"AssistantMessage.api", "ADR 0029/0033 provider identity, not conversational text"},
        Exemption{"AssistantMessage.provider", "ADR 0029/0033 provider identity, not conversational text"},
        Exemption{"AssistantMessage.model", "ADR 0029/0033 provider identity, not conversational text"},
        Exemption{"AssistantMessage.response_model", "Provider routing identity, not conversational text"},
        Exemption{"AssistantMessage.response_id", "Provider response identity, not conversational text"},
        Exemption{"AssistantMessage.raw_stop_reason", "Provider stop-reason vocabulary token"},
        Exemption{"AssistantMessage.content[].text_signature",
                "Opaque provider signature token; redaction could corrupt it"},
        Exemption{"AssistantMessage.content[].thinking_signature",
                "Opaque provider signature token; redaction could corrupt it"},
        Exemption{"AssistantMessage.content[].id", "Tool-call identifier"},
        Exemption{"AssistantMessage.content[].name", "Tool name, not free text"},
        Exemption{"AssistantMessage.content[].thought_signature",
                "Opaque provider signature token; redaction could corrupt it"},
        Exemption{
                "UserMessage.content[].text_signature", "Opaque provider signature token; redaction could corrupt it"},
        Exemption{"UserMessage.content[].thinking_signature",
                "Opaque provider signature token; redaction could corrupt it"},
        Exemption{"UserMessage.content[].data", "Binary image payload; redaction would corrupt it"},
        Exemption{"UserMessage.content[].mime_type", "Media type, not free text"},
        Exemption{"ToolResultMessage.tool_call_id", "Structured tool identifier"},
        Exemption{"ToolResultMessage.tool_name", "Tool name, not free text"},
        Exemption{"ToolResultMessage.content[].text_signature",
                "Opaque provider signature token; redaction could corrupt it"},
        Exemption{"ToolResultMessage.content[].thinking_signature",
                "Opaque provider signature token; redaction could corrupt it"},
        Exemption{"ToolResultMessage.content[].data", "Binary image payload; redaction would corrupt it"},
        Exemption{"ToolResultMessage.content[].mime_type", "Media type, not free text"},
        Exemption{"BashExecutionMessage.command", "ADR 0028:30 User Bash command is raw by accepted decision"},
        Exemption{"BashExecutionMessage.output",
                "ADR 0028 Output section \"No redaction\" for User Bash output; adjudicated in #677 option 甲 and "
                "applied under #679"},
        Exemption{"BashExecutionMessage.full_output_path", "Spill artifact path, not conversational text"},
        Exemption{"CustomMessage.custom_type", "Extension discriminator, not free text"},
        Exemption{"CustomMessage.content[].text_signature",
                "Opaque provider signature token; redaction could corrupt it"},
        Exemption{"CustomMessage.content[].thinking_signature",
                "Opaque provider signature token; redaction could corrupt it"},
        Exemption{"CustomMessage.content[].data", "Binary image payload; redaction would corrupt it"},
        Exemption{"CustomMessage.content[].mime_type", "Media type, not free text"},
        Exemption{"BranchSummaryMessage.from_id", "Entry identifier, not conversational text"},
};

[[nodiscard]] bool is_exempt(std::string_view path) {
    return std::ranges::any_of(kExemptions, [path](const Exemption& exemption) { return exemption.path == path; });
}

constexpr std::int64_t kRealUnixEpochMilliseconds = 1718000000123;

/// Drive one poisoned message through the real persistence entry point and
/// assert the write-side redaction contract for every registered leaf.
void check_redaction_coverage(const ai::MessageVariant& message, const PoisonRegistry& registry) {
    harness::session::EntrySerializer serializer;
    const auto line = serializer.serialize_message(message);
    REQUIRE(line);

    // The un-redacted wire form proves the sentinel actually reaches the DTO
    // mapping. Without it, a sentinel absent from the persisted line could
    // mean "redacted" or merely "never mapped" — the difference between the
    // property and a vacuous pass.
    const auto unredacted = harness::session::detail::to_message_dto(message);
    REQUIRE(unredacted);
    const auto unredacted_json = support::write_json(*unredacted);
    REQUIRE(unredacted_json);

    for (const auto& leaf : registry.leaves) {
        INFO("field: " << leaf.path);
        CHECK(unredacted_json->find(leaf.sentinel) != std::string::npos);
        if (is_exempt(leaf.path)) {
            CHECK(line->find(leaf.sentinel) != std::string::npos);
        } else {
            CHECK(line->find(leaf.sentinel) == std::string::npos);
        }
    }
}

struct PoisonedMessage {
    ai::MessageVariant message;
    PoisonRegistry registry;
};

/// Poison a copy of `message`, returning the poisoned value and every leaf it
/// registered.
[[nodiscard]] PoisonedMessage poison_message(const ai::MessageVariant& message, const std::string& root) {
    PoisonedMessage poisoned{message, {}};
    std::visit([&](auto& concrete) { poison_leaves(concrete, root, poisoned.registry); }, poisoned.message);
    return poisoned;
}

/// Visit one freshly built message per write-side role. The role list lives
/// here once so a role cannot be covered by one case and missed by another.
template <class Visitor> void for_each_role(const Visitor& visit) {
    visit(ai::MessageVariant{ai::SystemMessage{}}, "SystemMessage");
    visit(ai::MessageVariant{ai::UserMessage{}}, "UserMessage");
    // `UserMessage::content` is a bare variant: its block alternative is not
    // reachable from the default-constructed value.
    ai::UserMessage user_blocks;
    user_blocks.content = std::vector<ai::Content>{};
    visit(ai::MessageVariant{std::move(user_blocks)}, "UserMessage");
    ai::AssistantMessage assistant;
    assistant.timestamp = kRealUnixEpochMilliseconds;
    visit(ai::MessageVariant{assistant}, "AssistantMessage");
    visit(ai::MessageVariant{ai::ToolResultMessage{}}, "ToolResultMessage");
    visit(ai::MessageVariant{ai::BashExecutionMessage{}}, "BashExecutionMessage");
    visit(ai::MessageVariant{ai::CustomMessage{}}, "CustomMessage");
    visit(ai::MessageVariant{ai::BranchSummaryMessage{}}, "BranchSummaryMessage");
    visit(ai::MessageVariant{ai::CompactionSummaryMessage{}}, "CompactionSummaryMessage");
}

} // namespace

// A new message role must be added to the checks below, or the write-side set
// silently loses a role.
static_assert(std::variant_size_v<ai::MessageVariant> == 8,
        "#666: a message role was added or removed; add/remove its poisoned case in this test.");
static_assert(std::variant_size_v<decltype(ai::UserMessage::content)> == 2,
        "#666: UserMessage::content gained or lost an alternative; extend the redaction coverage test.");

TEST_CASE("Session write-path redaction covers every AI string field of every role",
        "[harness][session][issue666][spec]") {
    for_each_role([](const ai::MessageVariant& message, const std::string& root) {
        const auto poisoned = poison_message(message, root);
        // An unhandled leaf type means the reflection walk would silently skip
        // it, so the coverage below would not be complete.
        CHECK(poisoned.registry.unhandled.empty());
        check_redaction_coverage(poisoned.message, poisoned.registry);
    });
}

TEST_CASE("Session write-path redaction list has no stale exemptions", "[harness][session][issue666][spec]") {
    // Collect every write-side string leaf exactly as the coverage case does,
    // then require each exemption to name a real leaf. A renamed or removed
    // field would otherwise leave a dead exemption behind that silently
    // reauthorizes whatever reuses that spelling.
    std::vector<std::string> leaves;
    for_each_role([&leaves](const ai::MessageVariant& message, const std::string& root) {
        for (const auto& leaf : poison_message(message, root).registry.leaves) {
            leaves.push_back(leaf.path);
        }
    });

    for (const auto& exemption : kExemptions) {
        INFO("stale exemption: " << exemption.path);
        CHECK(std::ranges::find(leaves, exemption.path) != leaves.end());
    }
}

TEST_CASE(
        "Session redaction, not the DTO mapping, removes a diagnostic sentinel", "[harness][session][issue666][spec]") {
    // The counterexample #666 asks for, stated the other way round: the same
    // real value leaks its secret through the wire when the redaction step is
    // skipped. If a redaction were omitted from `redacted_message`, the
    // coverage case above would observe this surviving sentinel and fail.
    const std::string sentinel = sentinel_for("AssistantMessage.diagnostics[].error.stack");

    ai::AssistantMessage assistant;
    assistant.timestamp = kRealUnixEpochMilliseconds;
    assistant.api = "openai-completions";
    assistant.provider = "openai-compatible";
    assistant.model = "gpt-test";
    assistant.diagnostics.emplace();
    assistant.diagnostics->emplace_back();
    assistant.diagnostics->back().error = ai::DiagnosticErrorInfo{
            .name = std::nullopt,
            .message = "diagnostic",
            .stack = sentinel,
            .code = std::nullopt,
    };

    harness::session::EntrySerializer serializer;
    const auto line = serializer.serialize_message(ai::MessageVariant{assistant});
    REQUIRE(line);
    const auto unredacted = harness::session::detail::to_message_dto(ai::MessageVariant{assistant});
    REQUIRE(unredacted);
    const auto unredacted_json = support::write_json(*unredacted);
    REQUIRE(unredacted_json);

    // Skipping redaction alone is enough to leak; the detector sees it.
    CHECK(unredacted_json->find(sentinel) != std::string::npos);
    CHECK(line->find(sentinel) == std::string::npos);
}
