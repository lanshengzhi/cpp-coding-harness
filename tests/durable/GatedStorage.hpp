#pragma once

#include <cch/durable/Storage.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tests {

/// A `cch::durable::Storage` that delegates every real operation to an inner
/// `cch::durable::MemoryStorage` and adds two fault-injection controls the
/// frozen memory backend has no equivalent of: holding one commit at a latch
/// (the pre-settlement observation of `audit-1063` scenario "a prepared draft
/// is not observable before Storage settles its commit") and failing the next
/// commit (the frozen "disk vanished" backend failure).
///
/// The double is never the subject under test: the committed records, the ID
/// namespace and the sequences come from the real `MemoryStorage` behind it.
///
/// Latch contract (CODING_STANDARDS.md §11.8): the latch is counted and
/// latched. A release that arrives before the next commit arms the hold is
/// stored and consumed by that hold, so release/arm ordering cannot hang a
/// test. It holds no timer and never blocks: the held commit returns a pending
/// `AsyncResult` whose completion the test settles by releasing it, so the
/// frozen "nothing is observable before the storage settles" boundary is
/// observable without a second serialized domain.
class GatedStorage final : public ::cch::durable::Storage {
public:
    GatedStorage() : inner_(std::make_unique<::cch::durable::MemoryStorage>()) {}

    GatedStorage(const GatedStorage&) = delete;
    GatedStorage& operator=(const GatedStorage&) = delete;
    ~GatedStorage() override = default;

    /// Hold the next commit until `release_held_commit()` is called.
    void hold_next_commit() { hold_armed_ = true; }

    /// Release the held commit, running it against the inner storage. A
    /// release that arrives before a hold is armed is stored for the next
    /// hold.
    void release_held_commit() {
        if (!held_.has_value()) {
            permits_ += 1;
            return;
        }
        Held held{std::move(*held_)};
        held_.reset();
        run_inner_commit(std::move(held));
    }

    /// Fail the next commit with `message` and no durable effect.
    void fail_next_commit(std::string message) { failure_ = std::move(message); }

    /// The number of `commit()` calls this double received.
    [[nodiscard]] std::size_t commit_calls() const noexcept { return commit_calls_; }

    /// The number of `close()` calls this double received.
    [[nodiscard]] std::size_t close_calls() const noexcept { return close_calls_; }

    /// Whether a commit is held right now.
    [[nodiscard]] bool holds_commit() const noexcept { return held_.has_value(); }

    [[nodiscard]] support::AsyncResult<::cch::durable::Seq> commit(
            std::span<const ::cch::durable::StorageWrite> writes) override {
        commit_calls_ += 1;
        if (failure_.has_value()) {
            std::string message = std::move(*failure_);
            failure_.reset();
            return support::AsyncResult<::cch::durable::Seq>{support::Expected<::cch::durable::Seq>{
                    std::unexpected(support::make_error(support::ErrorCode::Process, std::move(message)))}};
        }
        if (hold_armed_) {
            hold_armed_ = false;
            std::vector<::cch::durable::StorageWrite> held_writes(writes.begin(), writes.end());
            return support::AsyncResult<::cch::durable::Seq>{
                    support::AsyncProducer<::cch::durable::Seq, support::Error>{
                            [this, held_writes = std::move(held_writes)](support::AsyncCompletion<::cch::durable::Seq,
                                    support::Error> completion) mutable noexcept {
                                held_ = Held{std::move(held_writes), std::move(completion)};
                            }}};
        }
        return inner_->commit(writes);
    }

    [[nodiscard]] support::Expected<std::int64_t> mint_id() override { return inner_->mint_id(); }

    [[nodiscard]] support::Expected<std::optional<::cch::durable::ConversationRecord>> conversation(
            ::cch::durable::ConversationId id) override {
        return inner_->conversation(id);
    }

    [[nodiscard]] support::Expected<std::optional<::cch::durable::StoredEntry>> entry(
            ::cch::durable::EntryId id) override {
        return inner_->entry(id);
    }

    [[nodiscard]] support::Expected<std::vector<::cch::durable::EntryRecord>> scan_entries(
            ::cch::durable::ConversationId conversation_id, std::size_t limit) override {
        return inner_->scan_entries(conversation_id, limit);
    }

    [[nodiscard]] support::Expected<std::vector<::cch::durable::ConversationRecord>> scan_conversations(
            std::size_t limit) override {
        return inner_->scan_conversations(limit);
    }

    [[nodiscard]] support::Expected<std::optional<::cch::durable::DocumentRecord>> find_document(
            std::string_view kind, std::optional<::cch::durable::ConversationId> conversation_id) override {
        return inner_->find_document(kind, conversation_id);
    }

    [[nodiscard]] support::Expected<std::optional<chord::Json>> document(::cch::durable::DocumentId id) override {
        return inner_->document(id);
    }

    [[nodiscard]] support::ExpectedVoid close() override {
        close_calls_ += 1;
        return inner_->close();
    }

private:
    struct Held {
        std::vector<::cch::durable::StorageWrite> writes;
        support::AsyncCompletion<::cch::durable::Seq, support::Error> completion;

        Held(std::vector<::cch::durable::StorageWrite> batch,
                support::AsyncCompletion<::cch::durable::Seq, support::Error> settle)
            : writes(std::move(batch)), completion(std::move(settle)) {}

        Held(Held&&) noexcept = default;
        Held& operator=(Held&&) noexcept = default;
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;
    };

    void run_inner_commit(Held held) {
        inner_->commit(held.writes)
                .start([completion = std::move(held.completion)](
                               std::expected<::cch::durable::Seq, support::Error> outcome) mutable noexcept {
                    completion(std::move(outcome));
                });
    }

    std::unique_ptr<::cch::durable::MemoryStorage> inner_;
    std::optional<Held> held_;
    std::optional<std::string> failure_;
    bool hold_armed_{false};
    std::size_t permits_{0};
    std::size_t commit_calls_{0};
    std::size_t close_calls_{0};
};

} // namespace cch::tests
