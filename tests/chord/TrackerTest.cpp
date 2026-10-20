#include <cch/chord/Delta.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
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

} // namespace

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "prepared revisions are immutable and a same-base preparation becomes stale after adoption".
TEST_CASE("a same-base preparation becomes stale after another preparation is adopted",
        "[chord][delta][revision][stale][issue1061][spec]") {
    auto tracker = track(object({{"value", Json{1}}, {"items", array({Json{"a"}})}}));
    const Json before = tracker.value();

    auto first = tracker.begin_change();
    REQUIRE(first.draft().set(keys({"value"}), Json{2}));
    auto first_prepared = first.prepare();
    REQUIRE(first_prepared);
    REQUIRE(first_prepared->ops().size() == 1);
    CHECK(std::holds_alternative<SetOp>(first_prepared->ops().front()));

    auto second = tracker.begin_change();
    REQUIRE(second.draft().set(keys({"value"}), Json{3}));
    auto second_prepared = second.prepare();
    REQUIRE(second_prepared);

    REQUIRE(tracker.adopt(*first_prepared));
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));
    CHECK(before == object({{"value", Json{1}}, {"items", array({Json{"a"}})}}));

    const auto stale = tracker.adopt(*second_prepared);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().message == "Prepared change is stale");
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));

    const auto used = tracker.adopt(*first_prepared);
    REQUIRE_FALSE(used);
    CHECK(used.error().message == "Prepared change has already been used");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "aborting a changed draft revokes escaped handles and leaves the committed revision unchanged".
TEST_CASE("aborting a prepared change revokes escaped draft handles",
        "[chord][delta][revision][abort][issue1061][spec]") {
    auto tracker = track(object({{"value", Json{1}}}));
    auto change = tracker.begin_change();
    auto escaped = change.draft();
    REQUIRE(escaped.set(keys({"value"}), Json{9}));
    auto prepared = change.prepare();
    REQUIRE(prepared);

    change.abort();
    const auto escaped_write = escaped.set(keys({"value"}), Json{10});
    REQUIRE_FALSE(escaped_write);
    CHECK(escaped_write.error().message == "Cannot use a settled overlay");
    const auto escaped_read = escaped.get(keys({"value"}));
    REQUIRE_FALSE(escaped_read);
    CHECK(escaped_read.error().message == "Cannot use a settled overlay");

    const auto abandoned = tracker.adopt(*prepared);
    REQUIRE_FALSE(abandoned);
    CHECK(abandoned.error().message == "Prepared change has been aborted");
    CHECK(tracker.value() == object({{"value", Json{1}}}));
    CHECK(tracker.revision() == 0);

    const auto settled = change.prepare();
    REQUIRE_FALSE(settled);
    CHECK(settled.error().message == "Change has already been settled");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1057/delta-revisions.json
// "a structural no-op emits a nonempty batch while an untouched change emits none and still adopts".
TEST_CASE("a structural no-op emits splice operations while an untouched change emits none",
        "[chord][delta][revision][noop][issue1061][spec]") {
    auto tracker = track(object({{"items", array({Json{"a"}})}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.splice(keys({"items"}), 0, 1, {}));
    REQUIRE(draft.splice(keys({"items"}), 0, 0, {Json{"a"}}));
    auto prepared = change.prepare();
    REQUIRE(prepared);
    REQUIRE(prepared->ops().size() == 2);
    const auto* remove = std::get_if<SpliceOp>(&prepared->ops()[0]);
    const auto* insert = std::get_if<SpliceOp>(&prepared->ops()[1]);
    REQUIRE(remove != nullptr);
    REQUIRE(insert != nullptr);
    CHECK(remove->index == 0);
    CHECK(remove->remove == 1);
    CHECK(insert->index == 0);
    CHECK(insert->remove == 0);
    REQUIRE(insert->items.size() == 1);
    CHECK(insert->items[0] == Json{"a"});
    REQUIRE(tracker.adopt(*prepared));
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == object({{"items", array({Json{"a"}})}}));

    auto empty_tracker = track(object({{"value", Json{1}}}));
    auto empty_change = empty_tracker.begin_change();
    auto empty_prepared = empty_change.prepare();
    REQUIRE(empty_prepared);
    CHECK(empty_prepared->ops().empty());
    CHECK(empty_prepared->value().shares_container_with(empty_tracker.value()));
    REQUIRE(empty_tracker.adopt(*empty_prepared));
    CHECK(empty_tracker.revision() == 1);
    CHECK(empty_tracker.value() == object({{"value", Json{1}}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/prepare-replace.json
// "a prepared whole-root replacement keeps the prior revision as its base and stales a same-base preparation after
// adoption".
TEST_CASE("prepare replace keeps the checkpoint base immutable and stales same-base preparations",
        "[chord][delta][revision][prepare-replace][issue1061][spec]") {
    const Json initial = object({{"value", Json{1}}, {"items", array({Json{"a"}})}});
    auto tracker = track(initial);
    const Json snapshot = tracker.value();

    const Json replacement = object({{"value", Json{2}}, {"items", array({Json{"a"}})}});
    auto prepared = tracker.prepare_replace(replacement);
    REQUIRE(prepared);
    CHECK(prepared->value().shares_container_with(replacement));
    CHECK(prepared->base() == initial);
    CHECK(prepared->value() == object({{"value", Json{2}}, {"items", array({Json{"a"}})}}));
    REQUIRE(prepared->ops().size() == 1);
    const auto* replace_op = std::get_if<ReplaceOp>(&prepared->ops().front());
    REQUIRE(replace_op != nullptr);
    CHECK(replace_op->value == prepared->value());
    CHECK(prepared->base_revision() == 0);
    CHECK(prepared->base().shares_container_with(tracker.value()));
    CHECK(tracker.revision() == 0);
    CHECK(tracker.value() == initial);

    auto competitor = tracker.begin_change();
    REQUIRE(competitor.draft().set(keys({"value"}), Json{3}));
    auto competitor_prepared = competitor.prepare();
    REQUIRE(competitor_prepared);

    REQUIRE(tracker.adopt(*prepared));
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == prepared->value());
    CHECK(snapshot == initial);
    CHECK(prepared->base() == initial);

    const auto stale = tracker.adopt(*competitor_prepared);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().message == "Prepared change is stale");
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == prepared->value());

    const auto used = tracker.adopt(*prepared);
    REQUIRE_FALSE(used);
    CHECK(used.error().message == "Prepared change has already been used");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/prepare-replace.json
// "a deeply equal whole-root replacement emits no operations and still stales a same-base preparation";
// packages/chord/src/delta/tracker.ts:Tracker.prepareReplace
// keeps the current root and emits no operations when the replacement is deeply equal;
// adopting that no-op still advances the revision and stales a same-base competitor.
TEST_CASE("prepare replace of a deeply equal root emits no operations and stales a competitor",
        "[chord][delta][revision][prepare-replace][noop][issue1061][spec]") {
    auto tracker = track(object({{"value", Json{1}}, {"items", array({Json{"a"}})}}));
    auto prepared = tracker.prepare_replace(object({{"value", Json{1}}, {"items", array({Json{"a"}})}}));
    auto competitor = tracker.prepare_replace(object({{"value", Json{1}}, {"items", array({Json{"a"}})}}));
    REQUIRE(prepared);
    REQUIRE(competitor);
    CHECK(prepared->ops().empty());
    CHECK(prepared->value().shares_container_with(tracker.value()));
    CHECK(prepared->base().shares_container_with(tracker.value()));
    REQUIRE(tracker.adopt(*prepared));
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == object({{"value", Json{1}}, {"items", array({Json{"a"}})}}));
    const auto stale = tracker.adopt(*competitor);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().message == "Prepared change is stale");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/prepare-replace.json
// "an aborted whole-root preparation cannot be adopted".
TEST_CASE("an aborted or foreign whole-root preparation cannot be adopted",
        "[chord][delta][revision][prepare-replace][abort][issue1061][spec]") {
    auto tracker = track(object({{"value", Json{1}}}));
    auto prepared = tracker.prepare_replace(object({{"value", Json{2}}}));
    REQUIRE(prepared);
    prepared->abort();
    const auto aborted = tracker.adopt(*prepared);
    REQUIRE_FALSE(aborted);
    CHECK(aborted.error().message == "Prepared change has been aborted");
    CHECK(tracker.value() == object({{"value", Json{1}}}));
    CHECK(tracker.revision() == 0);

    auto other = track(object({{"value", Json{10}}}));
    auto foreign = other.prepare_replace(object({{"value", Json{11}}}));
    REQUIRE(foreign);
    const auto rejected = tracker.adopt(*foreign);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message == "Prepared change belongs to a different tracker");
    CHECK(tracker.value() == object({{"value", Json{1}}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/draft-array-holes.json
// "deleting an array element from a draft rejects the hole and leaves the prepared revision empty".
TEST_CASE("draft paths reject unsafe empty and out-of-range mutations before recording",
        "[chord][delta][revision][validation][issue1061][spec]") {
    auto tracker = track(object({{"items", array({Json{"a"}})}, {"value", Json{1}}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();

    const auto unsafe = draft.set(Path{path_key("__proto__")}, Json{true});
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error().message == "unsafe path segment: __proto__");

    const auto empty = draft.set(Path{}, Json{true});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().message == "path is empty");

    const auto out_of_range = draft.erase(Path{path_key("items"), path_index(2)});
    REQUIRE_FALSE(out_of_range);
    CHECK(out_of_range.error().message == "Overlay arrays cannot contain holes");

    const auto array_delete = draft.erase(Path{path_key("items"), path_index(0)});
    REQUIRE_FALSE(array_delete);
    CHECK(array_delete.error().message == "Overlay arrays cannot contain holes");

    const auto bad_permutation = draft.move(keys({"items"}), {1, 1});
    REQUIRE_FALSE(bad_permutation);
    CHECK(bad_permutation.error().message == "m permutation is not a bijection");

    auto prepared = change.prepare();
    REQUIRE(prepared);
    CHECK(prepared->ops().empty());
    CHECK(prepared->value() == tracker.value());
}

TEST_CASE("draft reads see writes while the committed revision stays unchanged",
        "[chord][delta][revision][draft][issue1061][spec]") {
    auto tracker = track(object({{"value", Json{1}}, {"text", Json{"abc"}}}));
    const Json snapshot = tracker.value();
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.set(keys({"value"}), Json{2}));
    REQUIRE(draft.append_string(keys({"text"}), "d"));

    auto read = draft.get(keys({"value"}));
    REQUIRE(read);
    REQUIRE(read->has_value());
    CHECK(**read == Json{2});
    auto text = draft.get(keys({"text"}));
    REQUIRE(text);
    REQUIRE(text->has_value());
    CHECK(text->value().as_string() != nullptr);
    CHECK(*text->value().as_string() == "abcd");
    CHECK(tracker.value() == snapshot);
    CHECK(tracker.revision() == 0);

    const auto duplicate = draft.set(keys({"value"}), Json{2});
    REQUIRE(duplicate);
    auto prepared = change.prepare();
    REQUIRE(prepared);
    REQUIRE(prepared->ops().size() == 2);
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/review-fixes.json
// "draft reads distinguish absent leaves from null and from absent receiver paths".
TEST_CASE("draft reads distinguish absent leaves nulls and invalid traversal",
        "[chord][delta][revision][draft][read][issue1061][spec]") {
    auto tracker = track(object({
            {"present", Json{1}},
            {"nil", Json{nullptr}},
            {"object", object({{"present", Json{2}}})},
            {"items", array({Json{10}})},
    }));
    auto change = tracker.begin_change();
    auto draft = change.draft();

    auto present = draft.get(keys({"present"}));
    REQUIRE(present);
    REQUIRE(present->has_value());
    CHECK(**present == Json{1});

    auto missing_leaf = draft.get(keys({"missing"}));
    REQUIRE(missing_leaf);
    CHECK_FALSE(missing_leaf->has_value());

    auto null_leaf = draft.get(keys({"nil"}));
    REQUIRE(null_leaf);
    REQUIRE(null_leaf->has_value());
    CHECK(**null_leaf == Json{nullptr});

    auto nested_missing_leaf = draft.get(Path{path_key("object"), path_key("missing")});
    REQUIRE(nested_missing_leaf);
    CHECK_FALSE(nested_missing_leaf->has_value());

    auto missing_array_index = draft.get(Path{path_key("items"), path_index(2)});
    REQUIRE(missing_array_index);
    CHECK_FALSE(missing_array_index->has_value());

    const auto missing_receiver = draft.get(Path{path_key("missing"), path_key("deep")});
    REQUIRE_FALSE(missing_receiver);
    CHECK(missing_receiver.error().message.starts_with("unresolvable path"));

    const auto null_receiver = draft.get(Path{path_key("nil"), path_key("deep")});
    REQUIRE_FALSE(null_receiver);
    CHECK(null_receiver.error().message.starts_with("unresolvable path"));

    auto prepared = change.prepare();
    REQUIRE(prepared);
    const auto settled = draft.get(keys({"missing"}));
    REQUIRE_FALSE(settled);
    CHECK(settled.error().message == "Cannot use a settled overlay");
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/review-fixes.json
// "adoption immediately revokes competing open drafts while prepared competitors retain normal prepared abort and stale
// states".
TEST_CASE("adoption immediately revokes escaped competing draft handles",
        "[chord][delta][revision][adopt][draft][issue1061][spec]") {
    auto tracker = track(object({{"x", Json{1}}}));
    auto open = tracker.begin_change();
    auto open_draft = open.draft();
    REQUIRE(open_draft.set(keys({"x"}), Json{5}));

    auto prepared_competitor = tracker.begin_change();
    REQUIRE(prepared_competitor.draft().set(keys({"x"}), Json{6}));
    auto prepared = prepared_competitor.prepare();
    REQUIRE(prepared);
    auto aborted_competitor = tracker.prepare_replace(object({{"x", Json{3}}}));
    REQUIRE(aborted_competitor);

    auto winner = tracker.prepare_replace(object({{"x", Json{2}}}));
    REQUIRE(winner);
    REQUIRE(tracker.adopt(*winner));
    CHECK(tracker.revision() == 1);

    const auto open_prepare = open.prepare();
    REQUIRE_FALSE(open_prepare);
    CHECK(open_prepare.error().message == "Cannot use a settled overlay");

    const auto open_read = open_draft.get(keys({"x"}));
    REQUIRE_FALSE(open_read);
    CHECK(open_read.error().message == "Cannot use a settled overlay");

    const auto open_write = open_draft.set(keys({"x"}), Json{7});
    REQUIRE_FALSE(open_write);
    CHECK(open_write.error().message == "Cannot use a settled overlay");

    open.abort();
    const auto stale = tracker.adopt(*prepared);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().message == "Prepared change is stale");

    aborted_competitor->abort();
    const auto aborted = tracker.adopt(*aborted_competitor);
    REQUIRE_FALSE(aborted);
    CHECK(aborted.error().message == "Prepared change has been aborted");
    CHECK(tracker.value() == object({{"x", Json{2}}}));
}

// Frozen evidence: fixtures/pi-v1.3.0/audit-1061/review-fixes.json
// "adding a key and erasing it again normalizes to an empty batch over the unchanged base".
TEST_CASE("erasing a newly added key prepares an empty batch over the unchanged base",
        "[chord][delta][revision][ordering][normalize][issue1061][spec]") {
    auto tracker = track(object({{"a", Json{1}}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.set(keys({"added"}), object({{"leaf", Json{3}}})));
    REQUIRE(draft.erase(Path{path_key("added"), path_key("leaf")}));
    REQUIRE(draft.erase(keys({"added"})));

    auto prepared = change.prepare();
    REQUIRE(prepared);
    CHECK(prepared->ops().empty());
    CHECK(prepared->value().shares_container_with(tracker.value()));
    CHECK(prepared->value() == object({{"a", Json{1}}}));
    REQUIRE(tracker.adopt(*prepared));
    CHECK(tracker.revision() == 1);
    CHECK(tracker.value() == object({{"a", Json{1}}}));
}

// Frozen observation: packages/chord/src/delta/tracker.ts:emitObjectOperations
// encodes delete-and-readd explicitly; a later parent write supersedes its
// descendants and a repeated leaf write emits only the final value.
TEST_CASE("draft writes fold to the final leaf while delete and readd stays explicit",
        "[chord][delta][revision][ordering][issue1061][spec]") {
    auto tracker = track(object({{"o", object({{"a", Json{1}}})}, {"n", Json{1}}}));
    auto change = tracker.begin_change();
    auto draft = change.draft();
    REQUIRE(draft.erase(Path{path_key("o"), path_key("a")}));
    REQUIRE(draft.set(Path{path_key("o"), path_key("a")}, Json{1}));
    REQUIRE(draft.set(keys({"n"}), Json{2}));
    REQUIRE(draft.set(keys({"n"}), Json{1}));

    auto prepared = change.prepare();
    REQUIRE(prepared);
    REQUIRE(prepared->ops().size() == 2);
    const auto* deletion = std::get_if<DeleteOp>(&prepared->ops()[0]);
    const auto* set = std::get_if<SetOp>(&prepared->ops()[1]);
    REQUIRE(deletion != nullptr);
    REQUIRE(set != nullptr);
    CHECK(deletion->path == Path{path_key("o"), path_key("a")});
    CHECK(set->path == Path{path_key("o"), path_key("a")});
    CHECK(set->value == Json{1});
    REQUIRE(tracker.adopt(*prepared));
    CHECK(tracker.value() == object({{"o", object({{"a", Json{1}}})}, {"n", Json{1}}}));
}
