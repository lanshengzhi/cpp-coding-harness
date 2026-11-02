#pragma once

#include <cch/chord/Json.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::chord {

namespace detail {
struct ChangeState;
struct PreparedState;
struct TrackerState;
} // namespace detail

/// One validated path segment: an object key or a non-negative array index.
struct PathKey {
    std::string key;

    [[nodiscard]] bool operator==(const PathKey&) const = default;
};

struct PathIndex {
    std::size_t index{0};

    [[nodiscard]] bool operator==(const PathIndex&) const = default;
};

using PathSegment = std::variant<PathKey, PathIndex>;
using Path = std::vector<PathSegment>;

[[nodiscard]] inline PathSegment path_key(std::string key) { return PathKey{std::move(key)}; }

[[nodiscard]] inline PathSegment path_index(std::size_t index) { return PathIndex{index}; }

/// The seven frozen Chord delta operations. `ReplaceOp` is the only whole-root
/// operation; the path-bearing operations may not have an empty path. Equality
/// compares the decoded fields, so a batch a producer emitted can be compared
/// with the batch an ingestion boundary decoded.
struct ReplaceOp {
    Json value;

    [[nodiscard]] bool operator==(const ReplaceOp&) const = default;
};

struct SetOp {
    Path path;
    Json value;

    [[nodiscard]] bool operator==(const SetOp&) const = default;
};

struct DeleteOp {
    Path path;

    [[nodiscard]] bool operator==(const DeleteOp&) const = default;
};

struct AppendOp {
    Path path;
    std::string text;

    [[nodiscard]] bool operator==(const AppendOp&) const = default;
};

struct TruncateOp {
    Path path;
    std::size_t count{0};

    [[nodiscard]] bool operator==(const TruncateOp&) const = default;
};

struct SpliceOp {
    Path path;
    std::size_t index{0};
    std::size_t remove{0};
    std::vector<Json> items;

    [[nodiscard]] bool operator==(const SpliceOp&) const = default;
};

struct MoveOp {
    Path path;
    std::vector<std::size_t> permutation;

    [[nodiscard]] bool operator==(const MoveOp&) const = default;
};

using Op = std::variant<ReplaceOp, SetOp, DeleteOp, AppendOp, TruncateOp, SpliceOp, MoveOp>;
using OpBatch = std::vector<Op>;

[[nodiscard]] bool is_replace(const Op& op);

/// A batch begins with a root replacement (pi `isBase`).
[[nodiscard]] bool is_base(const OpBatch& ops);

/// Validate one operation's path safety, root-path prohibition and array
/// permutation shape. Payload strictness is the caller's ingestion contract,
/// matching frozen Chord's applier, which checks op shape but not payload JSON.
[[nodiscard]] support::ExpectedVoid validate_op(const Op& op);

/// Validate the complete batch before any target access.
[[nodiscard]] support::ExpectedVoid validate_batch(const OpBatch& ops);

/// Decode one frozen Chord op tuple (pi `Op`) into a typed operation.
///
/// This is the decoded-input ingestion boundary: paths are inline, there are no
/// path ids and no shortened tuples. It owns the frozen verb, arity and payload
/// rejection vocabulary (`unknown op verb: x`, `s arity`, `p index`, `p remove`,
/// `m permutation is not a bijection`, `path is empty`, `path is not an array`,
/// `unsafe path segment: <segment>`), so an untyped tuple that the typed `Op`
/// variant cannot represent is still rejected with its recorded message. A
/// payload value is not inspected beyond its shape, matching frozen `apply`.
[[nodiscard]] support::Expected<Op> decode_op(const Json& tuple);

/// Decode a complete frozen op-tuple array into a typed batch. The first
/// rejected tuple fails the whole batch, so no prefix of a rejected batch is
/// returned and no target is ever reached.
[[nodiscard]] support::Expected<OpBatch> decode_ops(const Json& tuples);

/// Apply decoded operations without changing `target`; untouched containers
/// are shared between `target` and the result.
[[nodiscard]] support::Expected<Json> apply_immutable(const Json& target, const OpBatch& ops);

/// Apply ordered immutable batches to one final revision. Earlier batches are
/// not exposed, and `target` is unchanged.
[[nodiscard]] support::Expected<Json> apply_immutable_batches(const Json& target, const std::vector<OpBatch>& batches);

/// Apply decoded operations to a caller-owned mutable value and return the
/// adopted root (pi `apply`). Operations are validated and executed
/// sequentially, so each successful operation before a root replacement stays
/// applied to `target` if a later operation fails.
///
/// A root replacement is adopted as a new root, never written into `target`:
/// the returned value shares the replacement payload's container and `target`
/// keeps the value it had when the replacement was reached. Without a root
/// replacement the returned value shares `target`'s root container. Shared input
/// containers must not be aliased by another revision.
[[nodiscard]] support::Expected<Json> apply(Json& target, const OpBatch& ops);

/// Longest suffix of `a` that is a prefix of `b`, bounded like pi `overlap`.
/// Lengths count UTF-16 code units, matching the frozen string contract.
[[nodiscard]] std::size_t overlap(std::string_view a,
        std::string_view b,
        std::size_t scan,
        std::size_t probe = 64,
        std::size_t max_candidates = 8);

/// A mutable transaction-scoped view of the tracker's immutable revision. A
/// draft is a handle: copies observe the same settlement, and every operation
/// fails with "Cannot use a settled overlay" after prepare or abort.
class Draft {
public:
    Draft(const Draft&) = default;
    Draft(Draft&&) noexcept = default;
    Draft& operator=(const Draft&) = default;
    Draft& operator=(Draft&&) noexcept = default;
    ~Draft() = default;

    [[nodiscard]] support::Expected<std::optional<Json>> get(const Path& path) const;
    [[nodiscard]] support::ExpectedVoid set(const Path& path, Json value);
    [[nodiscard]] support::ExpectedVoid erase(const Path& path);
    [[nodiscard]] support::ExpectedVoid append_string(const Path& path, std::string text);
    [[nodiscard]] support::ExpectedVoid truncate_string(const Path& path, std::size_t count);
    [[nodiscard]] support::ExpectedVoid splice(
            const Path& path, std::size_t index, std::size_t remove, std::vector<Json> items);
    [[nodiscard]] support::ExpectedVoid move(const Path& path, std::vector<std::size_t> permutation);

private:
    friend class Change;

    explicit Draft(std::shared_ptr<detail::ChangeState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::ChangeState> state_;
};

/// One prepared candidate revision (pi `Prepared`). The base and value are
/// immutable snapshots; adoption is a pointer swap that never changes either.
class Prepared {
public:
    Prepared(Prepared&&) noexcept = default;
    Prepared& operator=(Prepared&&) noexcept = default;
    ~Prepared() = default;
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;

    [[nodiscard]] const Json& base() const;
    [[nodiscard]] const Json& value() const;
    [[nodiscard]] const OpBatch& ops() const;
    [[nodiscard]] std::uint64_t base_revision() const;
    void abort() noexcept;

private:
    friend class Change;
    friend class Tracker;

    explicit Prepared(std::shared_ptr<detail::PreparedState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::PreparedState> state_;
};

/// One open draft transaction (pi `Change`). It may stay open across calls;
/// `prepare()` settles it and `abort()` revokes every escaped draft handle.
class Change {
public:
    Change(Change&&) noexcept = default;
    Change& operator=(Change&&) noexcept = default;
    ~Change() = default;
    Change(const Change&) = delete;
    Change& operator=(const Change&) = delete;

    [[nodiscard]] Draft draft() const;
    [[nodiscard]] support::Expected<Prepared> prepare();
    void abort() noexcept;

private:
    friend class Tracker;

    explicit Change(std::shared_ptr<detail::ChangeState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::ChangeState> state_;
};

/// One immutable revision sequence (pi `Tracker`). Every adoption advances
/// `revision`, including an empty batch; preparations from the prior revision
/// become stale rather than being rebased.
class Tracker {
public:
    Tracker(const Tracker&) = default;
    Tracker(Tracker&&) noexcept = default;
    Tracker& operator=(const Tracker&) = default;
    Tracker& operator=(Tracker&&) noexcept = default;
    ~Tracker() = default;

    [[nodiscard]] const Json& value() const;
    [[nodiscard]] std::uint64_t revision() const;
    [[nodiscard]] Change begin_change();

    /// Prepare a whole-root replacement. An equal root prepares no operations
    /// and keeps the current root; adopting it still advances the revision.
    /// This is a replacement, not a diff.
    [[nodiscard]] support::Expected<Prepared> prepare_replace(Json value);

    [[nodiscard]] support::ExpectedVoid adopt(const Prepared& prepared);

private:
    friend Tracker track(Json initial);

    explicit Tracker(std::shared_ptr<detail::TrackerState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::TrackerState> state_;
};

/// Take immutable ownership of a strict-JSON root (pi `track`).
[[nodiscard]] Tracker track(Json initial);

} // namespace cch::chord
