#include <cch/chord/Delta.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;
using namespace cch::chord;

namespace {

[[nodiscard]] Path keys(std::initializer_list<const char*> parts) {
    Path path;
    path.reserve(parts.size());
    for (const char* part : parts)
        path.push_back(path_key(part));
    return path;
}

[[nodiscard]] Json object(std::initializer_list<Json::Object::value_type> entries) {
    return Json{Json::Object{entries}};
}

[[nodiscard]] Json array(std::initializer_list<Json> entries) { return Json{Json::Array{entries}}; }

[[nodiscard]] const Json& member(const Json& value, const char* key) { return value.as_object()->at(key); }

template <typename Operation> [[nodiscard]] const Operation* operation_at(const OpBatch& ops, std::size_t index) {
    if (index >= ops.size()) return nullptr;
    return std::get_if<Operation>(&ops[index]);
}

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "immutable application reproduces each revision without mutating the base and shares untouched subtrees".
TEST_CASE("immutable application reproduces a revision while sharing untouched containers",
        "[chord][delta][apply][issue1061][spec]") {
    const Json base = object({
            {"value", Json{1}},
            {"nested",
                    object({
                            {"count", Json{0}},
                            {"retained", object({{"deep", Json{true}}})},
                    })},
            {"retained",
                    object({
                            {"list", array({Json{1}, Json{2}})},
                    })},
            {"items", array({Json{"a"}})},
    });
    const Json base_before = base;
    const OpBatch ops{
            SetOp{.path = keys({"value"}), .value = Json{2}},
            SetOp{.path = Path{path_key("nested"), path_key("count")}, .value = Json{1}},
            SpliceOp{.path = keys({"items"}), .index = 1, .remove = 0, .items = {Json{"b"}}},
    };

    auto next = apply_immutable(base, ops);
    REQUIRE(next);
    CHECK(base == base_before);
    CHECK(*next == object({
                           {"value", Json{2}},
                           {"nested",
                                   object({
                                           {"count", Json{1}},
                                           {"retained", object({{"deep", Json{true}}})},
                                   })},
                           {"retained",
                                   object({
                                           {"list", array({Json{1}, Json{2}})},
                                   })},
                           {"items", array({Json{"a"}, Json{"b"}})},
                   }));
    CHECK_FALSE(next->shares_container_with(base));
    CHECK(member(*next, "retained").shares_container_with(member(base, "retained")));
    CHECK(member(member(*next, "nested"), "retained")
                    .shares_container_with(member(member(base, "nested"), "retained")));

    auto next_again = apply_immutable(*next, OpBatch{SetOp{.path = keys({"value"}), .value = Json{3}}});
    REQUIRE(next_again);
    REQUIRE(member(*next_again, "value").as_number() != nullptr);
    CHECK(*member(*next_again, "value").as_number() == 3.0);
    CHECK(member(*next_again, "retained").shares_container_with(member(base, "retained")));
    // Frozen `nextAgain`: the earlier containers survive the later revision unchanged.
    CHECK(*next_again == object({
                                 {"value", Json{3}},
                                 {"nested",
                                         object({
                                                 {"count", Json{1}},
                                                 {"retained", object({{"deep", Json{true}}})},
                                         })},
                                 {"retained",
                                         object({
                                                 {"list", array({Json{1}, Json{2}})},
                                         })},
                                 {"items", array({Json{"a"}, Json{"b"}})},
                         }));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "batched immutable application applies ordered batches and returns only the final revision".
TEST_CASE("ordered immutable batches produce one final revision without changing the base",
        "[chord][delta][apply][batch][issue1061][spec]") {
    const Json base = object({{"items", array({Json{"a"}})}, {"value", Json{0}}});
    const std::vector<OpBatch> batches{
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{1}},
                    SpliceOp{.path = keys({"items"}), .index = 1, .remove = 0, .items = {Json{"b"}}},
            },
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    SpliceOp{.path = keys({"items"}), .index = 0, .remove = 1, .items = {}},
            },
    };

    auto final = apply_immutable_batches(base, batches);
    REQUIRE(final);
    CHECK(base == object({{"items", array({Json{"a"}})}, {"value", Json{0}}}));
    CHECK(*final == object({{"items", array({Json{"b"}})}, {"value", Json{2}}}));

    auto created = apply_immutable_batches(Json{}, {OpBatch{ReplaceOp{.value = object({{"created", Json{true}}})}}});
    REQUIRE(created);
    CHECK(*created == object({{"created", Json{true}}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "mutable application mutates in place except for a root replacement, which returns a new value".
TEST_CASE("mutable application mutates in place while a root replacement returns a new value",
        "[chord][delta][apply][replacement][issue1185][spec]") {
    Json target = object({{"items", array({Json{"a"}})}, {"value", Json{0}}});
    const OpBatch ops{
            SetOp{.path = keys({"value"}), .value = Json{1}},
            SpliceOp{.path = keys({"items"}), .index = 1, .remove = 0, .items = {Json{"b"}}},
    };

    auto applied = apply(target, ops);
    REQUIRE(applied);
    // sameTarget: the returned root is the caller's value, which was mutated in place.
    CHECK(applied->shares_container_with(target));
    CHECK(target == object({{"items", array({Json{"a"}, Json{"b"}})}, {"value", Json{1}}}));

    const Json replacement = object({{"replacement", Json{true}}});
    auto replaced = apply(target, OpBatch{ReplaceOp{.value = replacement}});
    REQUIRE(replaced);
    // replacementIsNew: the adopted root is a new value and the caller keeps its own.
    CHECK_FALSE(replaced->shares_container_with(target));
    CHECK(replaced->shares_container_with(replacement));
    CHECK(target == object({{"items", array({Json{"a"}, Json{"b"}})}, {"value", Json{1}}}));
    CHECK(*replaced == replacement);
    CHECK(is_base(OpBatch{ReplaceOp{.value = Json{nullptr}}}));
    CHECK(is_replace(Op{ReplaceOp{.value = Json{nullptr}}}));
    CHECK_FALSE(is_base(OpBatch{}));
    CHECK_FALSE(is_base(ops));
}

// A root replacement is adopted, never replayed as an incremental delta.
TEST_CASE("a root replacement keeps the preceding incremental write and adopts a new root",
        "[chord][delta][apply][replacement][issue1185][spec]") {
    Json target = object({{"value", Json{1}}});
    const Json replacement = object({{"replacement", Json{true}}});
    auto replaced = apply(target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    ReplaceOp{.value = replacement},
            });
    REQUIRE(replaced);
    CHECK(target == object({{"value", Json{2}}}));
    CHECK(*replaced == replacement);
    CHECK_FALSE(replaced->shares_container_with(target));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "unsafe prototype segments and malformed operation tuples are rejected before mutation";
// fixtures/pi-v1.3.0/audit-1061/validation-order.json
// "the operation validator rejects each representable malformed shape with its recorded message".
TEST_CASE("decoded operation ingestion rejects the frozen malformed shapes before any target access",
        "[chord][delta][decode][validation][issue1185][spec]") {
    auto unsafe = decode_ops(array({array({Json{"s"}, array({Json{"__proto__"}, Json{"polluted"}}), Json{true}})}));
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error().code == support::ErrorCode::Validation);
    CHECK(unsafe.error().message == "unsafe path segment: __proto__");

    auto unknown_verb = decode_op(array({Json{"x"}, array({Json{"value"}})}));
    REQUIRE_FALSE(unknown_verb);
    CHECK(unknown_verb.error().message == "unknown op verb: x");

    auto string_index = decode_ops(array({array({Json{"p"}, array({}), Json{"0"}, Json{0}, array({Json{"x"}})})}));
    REQUIRE_FALSE(string_index);
    CHECK(string_index.error().message == "p index");

    const std::vector<std::pair<Json, std::string>> shapes{
            {array({Json{"s"}, array({}), Json{1}}), "path is empty"},
            {array({Json{"d"}, array({})}), "path is empty"},
            {array({Json{"s"}, array({Json{"a"}})}), "s arity"},
            {array({Json{"s"}, array({Json{"a"}}), Json{1}, Json{2}}), "s arity"},
            {array({Json{"t"}, array({Json{"s"}}), Json{-1}}), "t shape"},
            {array({Json{"p"}, array({Json{"a"}}), Json{0}, Json{-1}, array({})}), "p remove"},
            {array({Json{"m"}, array({Json{"a"}}), array({Json{0}, Json{0}})}), "m permutation is not a bijection"},
            {array({Json{"x"}, array({Json{"a"}})}), "unknown op verb: x"},
    };
    for (const auto& [shape, message] : shapes) {
        auto decoded = decode_op(shape);
        REQUIRE_FALSE(decoded);
        CHECK(decoded.error().message == message);
    }

    // The frozen vocabulary is representable: every accepted tuple decodes to its
    // typed operation.
    auto accepted = decode_ops(array({
            array({Json{"s"}, array({Json{"a"}}), Json{1}}),
            array({Json{"d"}, array({Json{"a"}})}),
            array({Json{"a"}, array({Json{"a"}}), Json{"x"}}),
            array({Json{"t"}, array({Json{"a"}}), Json{1}}),
            array({Json{"p"}, array({}), Json{1}, Json{0}, array({Json{"x"}})}),
            array({Json{"m"}, array({}), array({Json{0}})}),
            array({Json{"r"}, object({{"r", Json{true}}})}),
    }));
    REQUIRE(accepted);
    REQUIRE(accepted->size() == 7);
    const auto* set = operation_at<SetOp>(*accepted, 0);
    const auto* deletion = operation_at<DeleteOp>(*accepted, 1);
    const auto* append = operation_at<AppendOp>(*accepted, 2);
    const auto* truncate = operation_at<TruncateOp>(*accepted, 3);
    const auto* splice = operation_at<SpliceOp>(*accepted, 4);
    const auto* move = operation_at<MoveOp>(*accepted, 5);
    const auto* replace = operation_at<ReplaceOp>(*accepted, 6);
    REQUIRE(set != nullptr);
    REQUIRE(deletion != nullptr);
    REQUIRE(append != nullptr);
    REQUIRE(truncate != nullptr);
    REQUIRE(splice != nullptr);
    REQUIRE(move != nullptr);
    REQUIRE(replace != nullptr);
    CHECK(*set == SetOp{.path = keys({"a"}), .value = Json{1}});
    CHECK(*deletion == DeleteOp{.path = keys({"a"})});
    CHECK(*append == AppendOp{.path = keys({"a"}), .text = "x"});
    CHECK(*truncate == TruncateOp{.path = keys({"a"}), .count = 1});
    CHECK(*splice == SpliceOp{.path = Path{}, .index = 1, .remove = 0, .items = {Json{"x"}}});
    CHECK(*move == MoveOp{.path = Path{}, .permutation = {0}});
    CHECK(*replace == ReplaceOp{.value = object({{"r", Json{true}}})});
}

// A seam that special-cased one reserved key, or that handed a caller the prefix
// of a rejected batch, would pass the frozen __proto__ case and fail these.
TEST_CASE("decoded ingestion rejects every reserved segment and never yields a partial batch",
        "[chord][delta][decode][validation][issue1185][spec]") {
    for (const char* segment : {"__proto__", "constructor", "prototype"}) {
        auto decoded = decode_op(array({Json{"s"}, array({Json{segment}, Json{"polluted"}}), Json{true}}));
        REQUIRE_FALSE(decoded);
        CHECK(decoded.error().message == std::format("unsafe path segment: {}", segment));
    }

    auto batch = decode_ops(array({
            array({Json{"s"}, array({Json{"value"}}), Json{2}}),
            array({Json{"p"}, array({Json{"items"}}), Json{"0"}, Json{0}, array({Json{"x"}})}),
    }));
    REQUIRE_FALSE(batch);
    CHECK(batch.error().message == "p index");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/validation-order.json
// "a later empty path keeps the preceding mutable write and reports the empty path";
// "a later unsafe path keeps the preceding mutable write and reports the unsafe segment";
// "a later string splice index keeps the preceding mutable write and reports the splice index";
// "a later non-bijective permutation keeps the preceding mutable write and reports the permutation";
// "a root replacement adopted before a failing operation leaves the caller object unchanged".
TEST_CASE("decoded application keeps the preceding mutable write when a later tuple is rejected",
        "[chord][delta][decode][validation][ordering][issue1185][spec]") {
    const Json initial = object({{"value", Json{1}}, {"items", array({Json{"a"}})}});

    Json empty_path = initial;
    const auto empty_path_failure = apply(empty_path,
            array({
                    array({Json{"s"}, array({Json{"value"}}), Json{2}}),
                    array({Json{"s"}, array({}), Json{3}}),
            }));
    REQUIRE_FALSE(empty_path_failure);
    CHECK(empty_path_failure.error().message == "path is empty");
    CHECK(empty_path == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));

    Json unsafe_path = object({{"value", Json{1}}});
    const auto unsafe_path_failure = apply(unsafe_path,
            array({
                    array({Json{"s"}, array({Json{"value"}}), Json{2}}),
                    array({Json{"s"}, array({Json{"__proto__"}, Json{"polluted"}}), Json{true}}),
            }));
    REQUIRE_FALSE(unsafe_path_failure);
    CHECK(unsafe_path_failure.error().message == "unsafe path segment: __proto__");
    CHECK(unsafe_path == object({{"value", Json{2}}}));

    Json string_index = initial;
    const auto string_index_failure = apply(string_index,
            array({
                    array({Json{"s"}, array({Json{"value"}}), Json{2}}),
                    array({Json{"p"}, array({Json{"items"}}), Json{"0"}, Json{0}, array({Json{"x"}})}),
            }));
    REQUIRE_FALSE(string_index_failure);
    CHECK(string_index_failure.error().message == "p index");
    CHECK(string_index == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));

    Json unknown_verb = object({{"value", Json{1}}});
    const auto unknown_verb_failure = apply(unknown_verb,
            array({
                    array({Json{"s"}, array({Json{"value"}}), Json{2}}),
                    array({Json{"x"}, array({Json{"value"}})}),
            }));
    REQUIRE_FALSE(unknown_verb_failure);
    CHECK(unknown_verb_failure.error().message == "unknown op verb: x");
    CHECK(unknown_verb == object({{"value", Json{2}}}));

    Json permutation = object({{"value", Json{1}}, {"a", array({Json{"x"}, Json{"y"}})}});
    const auto permutation_failure = apply(permutation,
            array({
                    array({Json{"s"}, array({Json{"value"}}), Json{2}}),
                    array({Json{"m"}, array({Json{"a"}}), array({Json{0}, Json{0}})}),
            }));
    REQUIRE_FALSE(permutation_failure);
    CHECK(permutation_failure.error().message == "m permutation is not a bijection");
    CHECK(permutation == object({{"value", Json{2}}, {"a", array({Json{"x"}, Json{"y"}})}}));

    // A root replacement adopted before the failing tuple leaves the caller object unchanged.
    Json replaced = object({{"value", Json{1}}});
    const auto replaced_failure = apply(replaced,
            array({
                    array({Json{"r"}, object({{"other", Json{2}}})}),
                    array({Json{"s"}, array({}), Json{3}}),
            }));
    REQUIRE_FALSE(replaced_failure);
    CHECK(replaced_failure.error().message == "path is empty");
    CHECK(replaced == object({{"value", Json{1}}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1054/session-transactions.json
// "document watches and states deliver only committed frames": the committed
// watch frame carried exactly this delta vocabulary, and applying it produced the
// delivered watch value.
TEST_CASE("a committed watch frame's frozen delta vocabulary matches the tracker draft and value",
        "[chord][delta][decode][vocabulary][issue1185][spec]") {
    const Json frozen_ops = array({
            array({Json{"s"}, array({Json{"text"}}), Json{"b"}}),
            array({Json{"s"}, array({Json{"revision"}}), Json{2}}),
    });
    const Json frozen_value = object({{"text", Json{"b"}}, {"revision", Json{2}}});

    auto decoded = decode_ops(frozen_ops);
    REQUIRE(decoded);
    CHECK_FALSE(is_base(*decoded));

    auto tracker = track(object({{"text", Json{"a"}}, {"revision", Json{1}}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.set(keys({"text"}), Json{"b"}));
    REQUIRE(draft.set(keys({"revision"}), Json{2}));
    auto prepared = change.prepare();
    REQUIRE(prepared);
    CHECK(prepared->ops() == *decoded);

    auto next = apply_immutable(tracker.value(), *decoded);
    REQUIRE(next);
    CHECK(*next == frozen_value);
}

// A root replacement is not the delta a committed watch frame delivers; a seam
// that replayed the frame as a replacement would produce a base batch instead.
TEST_CASE("a whole-root replacement is not the delta a committed watch frame delivers",
        "[chord][delta][decode][vocabulary][issue1185][spec]") {
    auto tracker = track(object({{"text", Json{"a"}}, {"revision", Json{1}}}));
    auto prepared = tracker.prepare_replace(object({{"text", Json{"b"}}, {"revision", Json{2}}}));
    REQUIRE(prepared);
    REQUIRE(prepared->ops().size() == 1);
    CHECK(is_base(prepared->ops()));
    CHECK(is_replace(prepared->ops().front()));

    auto decoded = decode_ops(array({
            array({Json{"s"}, array({Json{"text"}}), Json{"b"}}),
            array({Json{"s"}, array({Json{"revision"}}), Json{2}}),
    }));
    REQUIRE(decoded);
    CHECK(prepared->ops() != *decoded);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/validation-order.json
// records per-operation validation; the typed operations below cover the
// representable shapes, and "decoded application keeps the preceding mutable
// write when a later tuple is rejected" covers the malformed-tuple and
// string-index shapes through the decoded ingestion entry point.
TEST_CASE("application follows frozen sequential validation and replacement semantics",
        "[chord][delta][validation][issue1061][spec]") {
    Json target = object({{"value", Json{1}}, {"items", array({Json{"a"}})}});
    const Json before = target;

    const auto unsafe = apply_immutable(
            target, OpBatch{SetOp{.path = Path{path_key("__proto__"), path_key("polluted")}, .value = Json{true}}});
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error().message == "unsafe path segment: __proto__");

    const auto empty_path = apply_immutable(target, OpBatch{SetOp{.path = Path{}, .value = Json{true}}});
    REQUIRE_FALSE(empty_path);
    CHECK(empty_path.error().message == "path is empty");

    const auto malformed_move = apply_immutable(target, OpBatch{MoveOp{.path = Path{}, .permutation = {0, 0}}});
    REQUIRE_FALSE(malformed_move);
    CHECK(malformed_move.error().message == "m permutation is not a bijection");

    const auto missing_array_element =
            apply_immutable(target, OpBatch{DeleteOp{.path = Path{path_key("items"), path_index(3)}}});
    REQUIRE_FALSE(missing_array_element);
    CHECK(missing_array_element.error().message.starts_with("unresolvable path"));

    auto immutable_failure = apply_immutable(target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    SpliceOp{.path = keys({"items"}), .index = 0, .remove = 0, .items = {Json{"x"}}},
                    MoveOp{.path = keys({"items"}), .permutation = {0, 0}},
            });
    REQUIRE_FALSE(immutable_failure);
    CHECK(immutable_failure.error().message == "m permutation is not a bijection");
    CHECK(target == before);

    // Frozen validates operations in sequence; an earlier mutable write remains
    // applied when a later operation has an invalid path.
    const auto syntax_invalid = apply(target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    SetOp{.path = Path{path_key("__proto__"), path_key("value")}, .value = Json{3}},
            });
    REQUIRE_FALSE(syntax_invalid);
    CHECK(syntax_invalid.error().message == "unsafe path segment: __proto__");
    CHECK(target == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));

    Json permutation_target = object({{"value", Json{1}}, {"items", array({Json{"a"}, Json{"b"}})}});
    const auto invalid_permutation = apply(permutation_target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    MoveOp{.path = keys({"items"}), .permutation = {0, 0}},
            });
    REQUIRE_FALSE(invalid_permutation);
    CHECK(invalid_permutation.error().message == "m permutation is not a bijection");
    CHECK(permutation_target == object({{"value", Json{2}}, {"items", array({Json{"a"}, Json{"b"}})}}));

    // Frozen evidence: fixtures/pi-v1.3.0/audit-1061/review-fixes.json
    // "a later traversal failure keeps the preceding mutable write".
    // A syntactically valid leading operation is applied in place; a later
    // traversal failure does not roll it back.
    const auto partially_valid = apply(target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    SetOp{.path = Path{path_key("missing"), path_key("value")}, .value = Json{3}},
            });
    REQUIRE_FALSE(partially_valid);
    CHECK(partially_valid.error().message.starts_with("unresolvable path"));
    CHECK(target == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));

    const auto replaced_then_failed = apply(target,
            OpBatch{
                    ReplaceOp{.value = object({{"replacement", Json{true}}})},
                    SetOp{.path = keys({"missing", "value"}), .value = Json{3}},
            });
    REQUIRE_FALSE(replaced_then_failed);
    CHECK(replaced_then_failed.error().message.starts_with("unresolvable path"));
    CHECK(target == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));

    const Json before_root_replace = target;
    const auto replaced_then_empty = apply(target,
            OpBatch{
                    ReplaceOp{.value = object({{"replacement", Json{true}}})},
                    SetOp{.path = Path{}, .value = Json{3}},
            });
    REQUIRE_FALSE(replaced_then_empty);
    CHECK(replaced_then_empty.error().message == "path is empty");
    CHECK(target == before_root_replace);

    // Frozen evidence: fixtures/pi-v1.3.0/audit-1061/validation-order.json
    // "a later empty path keeps the preceding mutable write and reports the empty path".
    Json empty_path_target = object({{"value", Json{1}}, {"items", array({Json{"a"}})}});
    const auto write_then_empty_path = apply(empty_path_target,
            OpBatch{
                    SetOp{.path = keys({"value"}), .value = Json{2}},
                    SetOp{.path = Path{}, .value = Json{3}},
            });
    REQUIRE_FALSE(write_then_empty_path);
    CHECK(write_then_empty_path.error().message == "path is empty");
    CHECK(empty_path_target == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "the tracker emits the set, append, splice, permutation, delete, truncate, and replace vocabulary".
TEST_CASE("tracker drafts emit the frozen set append splice permutation delete truncate and replace vocabulary",
        "[chord][delta][tracker][vocabulary][issue1061][spec]") {
    SECTION("set") {
        auto tracker = track(object({{"o", object({{"a", Json{1}}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.set(Path{path_key("o"), path_key("a")}, Json{2}));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        const auto* set = operation_at<SetOp>(prepared->ops(), 0);
        REQUIRE(set != nullptr);
        CHECK(set->path == Path{path_key("o"), path_key("a")});
        CHECK(set->value == Json{2});
        REQUIRE(tracker.adopt(*prepared));
        CHECK(tracker.value() == object({{"o", object({{"a", Json{2}}})}}));
    }

    SECTION("append") {
        auto tracker = track(object({{"s", Json{"abc"}}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.append_string(keys({"s"}), "d"));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        const auto* append = operation_at<AppendOp>(prepared->ops(), 0);
        REQUIRE(append != nullptr);
        CHECK(append->path == keys({"s"}));
        CHECK(append->text == "d");
        REQUIRE(tracker.adopt(*prepared));
        CHECK(member(tracker.value(), "s").as_string() != nullptr);
        CHECK(*member(tracker.value(), "s").as_string() == "abcd");
    }

    SECTION("splice") {
        auto tracker = track(object({{"a", array({Json{"a"}, Json{"b"}, Json{"c"}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.splice(keys({"a"}), 1, 1, {Json{"X"}, Json{"Y"}}));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 2);
        const auto* remove = operation_at<SpliceOp>(prepared->ops(), 0);
        const auto* insert = operation_at<SpliceOp>(prepared->ops(), 1);
        REQUIRE(remove != nullptr);
        REQUIRE(insert != nullptr);
        CHECK(remove->index == 1);
        CHECK(remove->remove == 1);
        CHECK(remove->items.empty());
        CHECK(insert->index == 1);
        CHECK(insert->remove == 0);
        REQUIRE(insert->items.size() == 2);
        CHECK(insert->items[0] == Json{"X"});
        CHECK(insert->items[1] == Json{"Y"});
        REQUIRE(tracker.adopt(*prepared));
        CHECK(tracker.value() == object({{"a", array({Json{"a"}, Json{"X"}, Json{"Y"}, Json{"c"}})}}));
    }

    SECTION("permutation") {
        auto tracker = track(object({{"a", array({Json{"a"}, Json{"b"}, Json{"c"}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.move(keys({"a"}), {2, 1, 0}));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        const auto* permutation = operation_at<MoveOp>(prepared->ops(), 0);
        REQUIRE(permutation != nullptr);
        CHECK(permutation->permutation == std::vector<std::size_t>{2, 1, 0});
        REQUIRE(tracker.adopt(*prepared));
        CHECK(tracker.value() == object({{"a", array({Json{"c"}, Json{"b"}, Json{"a"}})}}));
    }

    SECTION("delete") {
        auto tracker = track(object({{"o", object({{"a", Json{1}}, {"b", Json{2}}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.erase(Path{path_key("o"), path_key("b")}));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        const auto* deletion = operation_at<DeleteOp>(prepared->ops(), 0);
        REQUIRE(deletion != nullptr);
        CHECK(deletion->path == Path{path_key("o"), path_key("b")});
        REQUIRE(tracker.adopt(*prepared));
        CHECK(tracker.value() == object({{"o", object({{"a", Json{1}}})}}));
    }

    SECTION("truncate") {
        auto tracker = track(object({{"s", Json{"abcdef"}}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.truncate_string(keys({"s"}), 3));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        const auto* truncate = operation_at<TruncateOp>(prepared->ops(), 0);
        REQUIRE(truncate != nullptr);
        CHECK(truncate->count == 3);
        REQUIRE(tracker.adopt(*prepared));
        CHECK(member(tracker.value(), "s").as_string() != nullptr);
        CHECK(*member(tracker.value(), "s").as_string() == "def");
    }

    SECTION("replace") {
        auto tracker = track(object({{"o", object({{"a", Json{1}}})}}));
        auto change = tracker.begin_change();
        auto draft = change.draft();
        REQUIRE(draft.set(keys({"o"}), object({{"b", Json{2}}})));
        auto prepared = change.prepare();
        REQUIRE(prepared);
        REQUIRE(prepared->ops().size() == 1);
        const auto* set = operation_at<SetOp>(prepared->ops(), 0);
        REQUIRE(set != nullptr);
        CHECK(set->path == keys({"o"}));
        CHECK(set->value == object({{"b", Json{2}}}));
        REQUIRE(tracker.adopt(*prepared));
        CHECK(tracker.value() == object({{"o", object({{"b", Json{2}}})}}));
    }
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "overlap returns the bounded common run used to merge tool output".
TEST_CASE("overlap returns the bounded common run", "[chord][delta][overlap][issue1061][spec]") {
    CHECK(overlap("abcdefgh", "defghxyz", 65'536) == 5);
    CHECK(overlap("abcdef", "defghi", 0) == 0);
    CHECK(overlap("", "defgh", 1) == 0);
    CHECK(overlap("abc", "abcd", 65'536) == 3);
}
