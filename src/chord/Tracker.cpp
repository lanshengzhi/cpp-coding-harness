#include <cch/chord/Delta.hpp>

#include "chord/DeltaInternal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cch::chord {

namespace detail {

struct OwnerToken {};

struct RevisionToken {};

enum class PreparedStatus {
    Prepared,
    Consumed,
    Aborted,
    Stale,
};

struct TrackerState {
    std::shared_ptr<const OwnerToken> owner{std::make_shared<const OwnerToken>()};
    std::shared_ptr<const RevisionToken> revision_token{std::make_shared<const RevisionToken>()};
    Json value;
    std::uint64_t revision{0};
};

struct PreparedState {
    std::shared_ptr<const OwnerToken> owner;
    std::shared_ptr<const RevisionToken> base_token;
    std::uint64_t base_revision{0};
    Json base;
    Json value;
    OpBatch ops;
    PreparedStatus status{PreparedStatus::Prepared};
};

struct ChangeState {
    std::shared_ptr<const OwnerToken> owner;
    std::shared_ptr<const RevisionToken> base_token;
    std::uint64_t base_revision{0};
    Json base;
    Json candidate;
    OpBatch recorded;
    bool structural{false};
    bool settled{false};
    std::shared_ptr<PreparedState> prepared;
};

} // namespace detail

namespace {

constexpr std::size_t kMaxDeltaOperations = 4'096;

[[nodiscard]] support::Error settled_overlay_error() {
    return support::make_error(support::ErrorCode::Validation, "Cannot use a settled overlay");
}

[[nodiscard]] support::Error path_error(const Path& path) {
    (void)path;
    return support::make_error(support::ErrorCode::Validation, "unresolvable path");
}

[[nodiscard]] bool is_write_op(const Op& op) {
    return std::holds_alternative<SetOp>(op) || std::holds_alternative<AppendOp>(op) ||
           std::holds_alternative<TruncateOp>(op);
}

[[nodiscard]] const Path& op_path(const Op& op) {
    return std::visit(
            [](const auto& value) -> const Path& {
                using Operation = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Operation, ReplaceOp>) {
                    static const Path empty;
                    return empty;
                } else {
                    return value.path;
                }
            },
            op);
}

[[nodiscard]] bool has_same_path(const Path& left, const Path& right) {
    return detail::path_is_prefix(left, right) && detail::path_is_prefix(right, left);
}

void emit_write(const detail::ChangeState& state, const Path& path, OpBatch& operations) {
    const auto candidate = detail::try_get_path(state.candidate, path);
    if (!candidate) return; // A later delete removed the written value.
    const auto base = detail::try_get_path(state.base, path);
    // A delete followed by the identical value must stay explicit so a replica
    // preserves the frozen delete-and-readd operation order.
    bool force_set = false;
    for (const auto& recorded : state.recorded) {
        const auto* deletion = std::get_if<DeleteOp>(&recorded);
        if (deletion != nullptr && has_same_path(deletion->path, path)) force_set = true;
    }
    if (!force_set && base && *base == *candidate) return;
    if (!force_set && base != nullptr) {
        if (const auto* before = base->as_string(); before != nullptr) {
            if (const auto* after = candidate->as_string(); after != nullptr) {
                detail::emit_string_change(*before, *after, path, operations);
                return;
            }
        }
    }
    operations.push_back(SetOp{.path = path, .value = *candidate});
}

/// Fold the final write state into the smallest exact batch. The frozen
/// tracker writes leaf paths; a later parent write covers its descendants and
/// a repeated write to the same path replaces the earlier one.
[[nodiscard]] OpBatch collapse_ops(const detail::ChangeState& state) {
    std::vector<std::size_t> writes;
    for (std::size_t index = 0; index < state.recorded.size(); ++index) {
        if (is_write_op(state.recorded[index])) writes.push_back(index);
    }
    std::vector<bool> write_survives(writes.size(), true);
    for (std::size_t left = 0; left < writes.size(); ++left) {
        const Path& left_path = op_path(state.recorded[writes[left]]);
        for (std::size_t right = 0; right < writes.size(); ++right) {
            if (left == right) continue;
            const Path& right_path = op_path(state.recorded[writes[right]]);
            if (has_same_path(left_path, right_path) && right > left) {
                write_survives[left] = false;
                break;
            }
            if (detail::path_is_prefix(right_path, left_path) && right_path.size() < left_path.size()) {
                write_survives[left] = false;
                break;
            }
        }
    }

    std::vector<bool> write_kept(state.recorded.size(), false);
    for (std::size_t index = 0; index < writes.size(); ++index) {
        if (write_survives[index]) write_kept[writes[index]] = true;
    }

    std::vector<bool> delete_kept(state.recorded.size(), false);
    for (std::size_t index = 0; index < state.recorded.size(); ++index) {
        const auto* deletion = std::get_if<DeleteOp>(&state.recorded[index]);
        if (deletion == nullptr) continue;
        bool covered = false;
        for (std::size_t write = 0; write < writes.size(); ++write) {
            if (!write_survives[write]) continue;
            const Path& write_path = op_path(state.recorded[writes[write]]);
            if (detail::path_is_prefix(write_path, deletion->path) && write_path.size() < deletion->path.size()) {
                covered = true;
                break;
            }
        }
        delete_kept[index] = !covered;
    }

    OpBatch operations;
    for (std::size_t index = 0; index < state.recorded.size(); ++index) {
        if (write_kept[index]) {
            emit_write(state, op_path(state.recorded[index]), operations);
        } else if (delete_kept[index]) {
            if (const auto* deletion = std::get_if<DeleteOp>(&state.recorded[index])) operations.push_back(*deletion);
        }
    }
    return operations;
}

[[nodiscard]] OpBatch recorded_ops(const detail::ChangeState& state) {
    OpBatch operations = state.recorded;
    if (operations.size() > kMaxDeltaOperations) operations = OpBatch{ReplaceOp{.value = state.candidate}};
    return operations;
}

} // namespace

support::Expected<Json> Draft::get(const Path& path) const {
    if (!state_ || state_->settled) return std::unexpected(settled_overlay_error());
    if (auto safe = detail::validate_safe_path(path); !safe) return std::unexpected(std::move(safe.error()));
    return detail::get_path(state_->candidate, path);
}

support::ExpectedVoid Draft::set(const Path& path, Json value) {
    if (!state_ || state_->settled) return std::unexpected(settled_overlay_error());
    if (path.empty()) return std::unexpected(support::make_error(support::ErrorCode::Validation, "path is empty"));
    if (auto safe = detail::validate_safe_path(path); !safe) return safe;
    const auto current = detail::try_get_path(state_->candidate, path);
    if (current && *current == value) return {};
    Json recorded_value = value;
    auto updated = detail::set_path(state_->candidate, path, std::move(value));
    if (!updated) return std::unexpected(std::move(updated.error()));
    state_->recorded.push_back(SetOp{.path = path, .value = std::move(recorded_value)});
    state_->candidate = std::move(*updated);
    return {};
}

support::ExpectedVoid Draft::erase(const Path& path) {
    if (!state_ || state_->settled) return std::unexpected(settled_overlay_error());
    if (path.empty()) return std::unexpected(support::make_error(support::ErrorCode::Validation, "path is empty"));
    if (auto safe = detail::validate_safe_path(path); !safe) return safe;
    const Path parent(path.begin(), path.end() - 1);
    if (const auto parent_value = detail::try_get_path(state_->candidate, parent);
            parent_value && parent_value->as_array() != nullptr) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Overlay arrays cannot contain holes"));
    }
    bool changed = false;
    auto updated = detail::erase_path(state_->candidate, path, changed);
    if (!updated) return std::unexpected(std::move(updated.error()));
    if (!changed) return {};
    state_->recorded.push_back(DeleteOp{.path = path});
    state_->candidate = std::move(*updated);
    return {};
}

support::ExpectedVoid Draft::append_string(const Path& path, std::string text) {
    if (!state_ || state_->settled) return std::unexpected(settled_overlay_error());
    if (path.empty()) return std::unexpected(support::make_error(support::ErrorCode::Validation, "path is empty"));
    if (auto safe = detail::validate_safe_path(path); !safe) return safe;
    auto current = detail::get_path(state_->candidate, path);
    if (!current) return std::unexpected(std::move(current.error()));
    const auto* string = current->as_string();
    if (string == nullptr) return std::unexpected(path_error(path));
    if (text.empty()) return {};
    std::string appended = *string;
    appended += text;
    auto updated = detail::set_path(state_->candidate, path, Json{std::move(appended)});
    if (!updated) return std::unexpected(std::move(updated.error()));
    state_->recorded.push_back(AppendOp{.path = path, .text = std::move(text)});
    state_->candidate = std::move(*updated);
    return {};
}

support::ExpectedVoid Draft::truncate_string(const Path& path, std::size_t count) {
    if (!state_ || state_->settled) return std::unexpected(settled_overlay_error());
    if (path.empty()) return std::unexpected(support::make_error(support::ErrorCode::Validation, "path is empty"));
    if (auto safe = detail::validate_safe_path(path); !safe) return safe;
    auto current = detail::get_path(state_->candidate, path);
    if (!current) return std::unexpected(std::move(current.error()));
    if (current->as_string() == nullptr) return std::unexpected(path_error(path));
    if (count == 0) return {};
    auto updated = detail::truncate_string_path(state_->candidate, path, count);
    if (!updated) return std::unexpected(std::move(updated.error()));
    if (*updated == state_->candidate) return {};
    state_->recorded.push_back(TruncateOp{.path = path, .count = count});
    state_->candidate = std::move(*updated);
    return {};
}

support::ExpectedVoid Draft::splice(const Path& path, std::size_t index, std::size_t remove, std::vector<Json> items) {
    if (!state_ || state_->settled) return std::unexpected(settled_overlay_error());
    if (auto safe = detail::validate_safe_path(path); !safe) return safe;
    auto target = detail::get_path(state_->candidate, path);
    if (!target) return std::unexpected(std::move(target.error()));
    const auto* array = target->as_array();
    if (array == nullptr) return std::unexpected(path_error(path));
    const std::size_t effective_index = std::min(index, array->size());
    const std::size_t effective_remove = std::min(remove, array->size() - effective_index);
    if (effective_remove == 0 && items.empty()) return {};
    std::size_t applied_index = 0;
    std::size_t applied_remove = 0;
    auto updated = detail::splice_path(
            state_->candidate, path, effective_index, effective_remove, items, applied_index, applied_remove);
    if (!updated) return std::unexpected(std::move(updated.error()));
    state_->structural = true;
    if (applied_remove != 0) {
        state_->recorded.push_back(
                SpliceOp{.path = path, .index = applied_index, .remove = applied_remove, .items = {}});
    }
    if (!items.empty()) {
        state_->recorded.push_back(
                SpliceOp{.path = path, .index = applied_index, .remove = 0, .items = std::move(items)});
    }
    state_->candidate = std::move(*updated);
    return {};
}

support::ExpectedVoid Draft::move(const Path& path, std::vector<std::size_t> permutation) {
    if (!state_ || state_->settled) return std::unexpected(settled_overlay_error());
    if (auto safe = detail::validate_safe_path(path); !safe) return safe;
    if (auto valid = detail::validate_permutation(permutation); !valid) return valid;
    auto target = detail::get_path(state_->candidate, path);
    if (!target) return std::unexpected(std::move(target.error()));
    const auto* array = target->as_array();
    if (array == nullptr || array->size() != permutation.size()) return std::unexpected(path_error(path));
    if (detail::path_is_identity_permutation(permutation)) return {};
    auto updated = detail::move_path(state_->candidate, path, permutation);
    if (!updated) return std::unexpected(std::move(updated.error()));
    state_->structural = true;
    state_->recorded.push_back(MoveOp{.path = path, .permutation = std::move(permutation)});
    state_->candidate = std::move(*updated);
    return {};
}

Draft Change::draft() const { return Draft(state_); }

support::Expected<Prepared> Change::prepare() {
    if (!state_ || state_->settled) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Change has already been settled"));
    }
    OpBatch operations = state_->structural ? recorded_ops(*state_) : collapse_ops(*state_);
    if (operations.size() > kMaxDeltaOperations) operations = OpBatch{ReplaceOp{.value = state_->candidate}};
    auto prepared = std::make_shared<detail::PreparedState>();
    prepared->owner = state_->owner;
    prepared->base_token = state_->base_token;
    prepared->base_revision = state_->base_revision;
    prepared->base = state_->base;
    prepared->value = operations.empty() ? state_->base : state_->candidate;
    prepared->ops = std::move(operations);
    state_->settled = true;
    state_->prepared = prepared;
    return Prepared(std::move(prepared));
}

void Change::abort() noexcept {
    if (!state_) return;
    if (!state_->settled) {
        state_->settled = true;
        return;
    }
    if (state_->prepared && state_->prepared->status == detail::PreparedStatus::Prepared) {
        state_->prepared->status = detail::PreparedStatus::Aborted;
    }
}

const Json& Tracker::value() const { return state_->value; }

std::uint64_t Tracker::revision() const { return state_->revision; }

Change Tracker::begin_change() {
    auto change = std::make_shared<detail::ChangeState>();
    change->owner = state_->owner;
    change->base_token = state_->revision_token;
    change->base_revision = state_->revision;
    change->base = state_->value;
    change->candidate = state_->value;
    return Change(std::move(change));
}

support::Expected<Prepared> Tracker::prepare_replace(Json value) {
    auto prepared = std::make_shared<detail::PreparedState>();
    prepared->owner = state_->owner;
    prepared->base_token = state_->revision_token;
    prepared->base_revision = state_->revision;
    prepared->base = state_->value;
    const bool equal = prepared->base == value;
    prepared->value = equal ? prepared->base : value;
    if (!equal) prepared->ops.push_back(ReplaceOp{.value = std::move(value)});
    return Prepared(std::move(prepared));
}

support::ExpectedVoid Tracker::adopt(const Prepared& prepared) {
    if (!prepared.state_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Prepared change is not ready"));
    }
    auto& candidate = *prepared.state_;
    if (candidate.owner != state_->owner) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Prepared change belongs to a different tracker"));
    }
    if (candidate.status == detail::PreparedStatus::Consumed) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Prepared change has already been used"));
    }
    if (candidate.status == detail::PreparedStatus::Aborted) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Prepared change has been aborted"));
    }
    if (candidate.status == detail::PreparedStatus::Stale) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Prepared change is stale"));
    }
    if (candidate.status != detail::PreparedStatus::Prepared) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Prepared change is not ready"));
    }
    if (candidate.base_revision != state_->revision || candidate.base_token != state_->revision_token) {
        candidate.status = detail::PreparedStatus::Stale;
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "Prepared change is stale"));
    }
    state_->value = candidate.value;
    state_->revision_token = std::make_shared<const detail::RevisionToken>();
    state_->revision += 1;
    candidate.status = detail::PreparedStatus::Consumed;
    return {};
}

const Json& Prepared::base() const { return state_->base; }

const Json& Prepared::value() const { return state_->value; }

const OpBatch& Prepared::ops() const { return state_->ops; }

std::uint64_t Prepared::base_revision() const { return state_->base_revision; }

void Prepared::abort() noexcept {
    if (!state_) return;
    if (state_->status == detail::PreparedStatus::Prepared) state_->status = detail::PreparedStatus::Aborted;
}

Tracker track(Json initial) {
    auto state = std::make_shared<detail::TrackerState>();
    state->value = std::move(initial);
    return Tracker(std::move(state));
}

} // namespace cch::chord
