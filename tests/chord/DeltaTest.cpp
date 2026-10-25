#include <cch/chord/Delta.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
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

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/validation-order.json
// "a root replacement adopted before a failing operation leaves the caller object unchanged".
TEST_CASE("mutable application exposes the replacement result and batch classifications",
        "[chord][delta][apply][issue1061][spec]") {
    Json target = object({{"items", array({Json{"a"}})}, {"value", Json{0}}});
    const OpBatch ops{
            SetOp{.path = keys({"value"}), .value = Json{1}},
            SpliceOp{.path = keys({"items"}), .index = 1, .remove = 0, .items = {Json{"b"}}},
    };

    auto applied = apply(target, ops);
    REQUIRE(applied);
    CHECK(target == object({{"items", array({Json{"a"}, Json{"b"}})}, {"value", Json{1}}}));

    auto replaced = apply(target, OpBatch{ReplaceOp{.value = object({{"replacement", Json{true}}})}});
    REQUIRE(replaced);
    CHECK(target == object({{"replacement", Json{true}}}));
    CHECK(is_base(OpBatch{ReplaceOp{.value = Json{nullptr}}}));
    CHECK(is_replace(Op{ReplaceOp{.value = Json{nullptr}}}));
    CHECK_FALSE(is_base(OpBatch{}));
    CHECK_FALSE(is_base(OpBatch{SetOp{.path = keys({"value"}), .value = Json{1}}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/validation-order.json
// records per-operation validation; typed C++ operations cannot represent the
// malformed-tuple and string-index cases, so checks below cover representable cases.
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
