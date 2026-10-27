#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::support {

/// One JSON object as an insertion-ordered sequence of members (#1149).
///
/// JavaScript's `JSON.parse`/`JSON.stringify` and `Object.entries` preserve the
/// source text's member order, and pi's durable prompt planning reads
/// `pi.system` message `sections` through `Object.entries` (#1083, #1149). A
/// sorted associative container cannot carry that order, so the shared JSON
/// value keeps members in the order they were parsed or inserted.
///
/// The surface is the subset of `std::map<std::string, T>` the repository
/// actually uses, so a member lookup reads the same at every call site:
/// `emplace`, `try_emplace`, `insert`, `insert_or_assign`, `erase`, `find`,
/// `contains`, `count`, `at`, `operator[]`, and iteration. Iteration yields
/// members in insertion order; `emplace` on an existing key keeps the original
/// position and its value (the `std::map` contract), while `insert_or_assign`
/// keeps the original position and replaces the value.
///
/// Member counts in this repository are small (median 2, 91.6% at most 8, 99.4%
/// at most 16, measured over the committed fixtures), so lookup is a linear
/// scan over a contiguous vector. That keeps the type a rule-of-zero value with
/// no index to invalidate, and it makes an object cheap to copy — which the
/// immutable `chord::Json` copy-on-write update path relies on.
///
/// Equality compares members by key and value, not by position: object member
/// order is not part of a JSON value (RFC 8259 §4). Use `same_order` where the
/// order itself is the fact being compared.
template <typename T> class JsonObject {
public:
    using key_type = std::string;
    using mapped_type = T;
    using value_type = std::pair<std::string, T>;
    using container_type = std::vector<value_type>;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = value_type&;
    using const_reference = const value_type&;
    using iterator = typename container_type::iterator;
    using const_iterator = typename container_type::const_iterator;
    using reverse_iterator = typename container_type::reverse_iterator;
    using const_reverse_iterator = typename container_type::const_reverse_iterator;

    JsonObject() = default;
    JsonObject(std::initializer_list<value_type> members) : members_(members) {
        // `std::map`'s initializer-list constructor keeps the first value for a
        // repeated key, so a duplicate must not displace the earlier member.
        deduplicate();
    }

    iterator begin() noexcept { return members_.begin(); }
    const_iterator begin() const noexcept { return members_.begin(); }
    const_iterator cbegin() const noexcept { return members_.cbegin(); }
    iterator end() noexcept { return members_.end(); }
    const_iterator end() const noexcept { return members_.end(); }
    const_iterator cend() const noexcept { return members_.cend(); }
    reverse_iterator rbegin() noexcept { return members_.rbegin(); }
    const_reverse_iterator rbegin() const noexcept { return members_.rbegin(); }
    reverse_iterator rend() noexcept { return members_.rend(); }
    const_reverse_iterator rend() const noexcept { return members_.rend(); }

    [[nodiscard]] bool empty() const noexcept { return members_.empty(); }
    [[nodiscard]] size_type size() const noexcept { return members_.size(); }

    void reserve(size_type count) { members_.reserve(count); }
    void clear() noexcept { members_.clear(); }

    // Lookup. Heterogeneous by design: every key-taking operation accepts a
    // `std::string_view`, which the previous `std::map<std::string, T>` with the
    // default `std::less<std::string>` comparator did not.
    iterator find(std::string_view key) noexcept {
        return members_.begin() + static_cast<difference_type>(index_of(key));
    }

    const_iterator find(std::string_view key) const noexcept {
        return members_.begin() + static_cast<difference_type>(index_of(key));
    }

    [[nodiscard]] bool contains(std::string_view key) const noexcept { return index_of(key) != members_.size(); }

    [[nodiscard]] size_type count(std::string_view key) const noexcept { return contains(key) ? 1 : 0; }

    /// Member access. A missing key terminates, as `std::map::at` did: callers
    /// either established presence with `find`/`contains` or hold a typed error
    /// path, and this project builds without exceptions.
    T& at(std::string_view key) {
        const auto position = index_of(key);
        if (position == members_.size()) {
            std::terminate();
        }
        return members_[position].second;
    }

    const T& at(std::string_view key) const {
        const auto position = index_of(key);
        if (position == members_.size()) {
            std::terminate();
        }
        return members_[position].second;
    }

    /// Insert-or-default, appending a new member at the end when absent.
    T& operator[](std::string key) {
        const auto position = index_of(key);
        if (position != members_.size()) {
            return members_[position].second;
        }
        members_.emplace_back(std::move(key), T{});
        return members_.back().second;
    }

    // Modifiers.
    std::pair<iterator, bool> emplace(std::string key, T value) {
        const auto position = index_of(key);
        if (position != members_.size()) {
            return {members_.begin() + static_cast<difference_type>(position), false};
        }
        members_.emplace_back(std::move(key), std::move(value));
        return {members_.end() - 1, true};
    }

    std::pair<iterator, bool> try_emplace(std::string key, T value) {
        return emplace(std::move(key), std::move(value));
    }

    std::pair<iterator, bool> insert(value_type value) {
        return emplace(std::move(value.first), std::move(value.second));
    }

    /// Keep an existing member's position, replace its value; append when absent.
    std::pair<iterator, bool> insert_or_assign(std::string key, T value) {
        const auto position = index_of(key);
        if (position != members_.size()) {
            members_[position].second = std::move(value);
            return {members_.begin() + static_cast<difference_type>(position), false};
        }
        members_.emplace_back(std::move(key), std::move(value));
        return {members_.end() - 1, true};
    }

    /// Erase by position, returning the iterator that follows the erased member.
    iterator erase(const_iterator position) {
        const auto offset = position - members_.cbegin();
        return members_.erase(members_.begin() + offset);
    }

    size_type erase(std::string_view key) {
        const auto position = index_of(key);
        if (position == members_.size()) {
            return 0;
        }
        members_.erase(members_.begin() + static_cast<difference_type>(position));
        return 1;
    }

    void swap(JsonObject& other) noexcept { members_.swap(other.members_); }

    /// Member equality by key and value. Order is not part of the value.
    ///
    /// Only available when the mapped type is equality-comparable, matching
    /// `std::map::operator==`, which was likewise only instantiated on use.
    /// `support::JsonValue` deliberately has no equality operator, so its
    /// objects are compared through `support::canonical_json` text instead.
    [[nodiscard]] bool operator==(const JsonObject& other) const
        requires requires(const T& left, const T& right) { left == right; }
    {
        if (members_.size() != other.members_.size()) {
            return false;
        }
        for (const auto& [key, value] : members_) {
            const auto position = other.index_of(key);
            if (position == other.members_.size() || !(other.members_[position].second == value)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool operator!=(const JsonObject& other) const
        requires requires(const T& left, const T& right) { left == right; }
    {
        return !(*this == other);
    }

    /// Whether both objects carry the same members in the same order. This is
    /// the order-sensitive fact, named so it is never implied by `operator==`.
    [[nodiscard]] bool same_order(const JsonObject& other) const
        requires requires(const T& left, const T& right) { left == right; }
    {
        return members_ == other.members_;
    }

private:
    [[nodiscard]] size_type index_of(std::string_view key) const noexcept {
        for (size_type position = 0; position < members_.size(); ++position) {
            if (members_[position].first == key) {
                return position;
            }
        }
        return members_.size();
    }

    void deduplicate() {
        container_type unique;
        unique.reserve(members_.size());
        for (auto& member : members_) {
            const bool seen = std::any_of(unique.begin(), unique.end(), [&member](const value_type& existing) {
                return existing.first == member.first;
            });
            if (!seen) {
                unique.push_back(std::move(member));
            }
        }
        members_ = std::move(unique);
    }

    container_type members_{};
};

} // namespace cch::support
