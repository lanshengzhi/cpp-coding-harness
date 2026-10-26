#pragma once

#include <cch/ai/Models.hpp>
#include <cch/durable/Records.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/JsonValue.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::durable {

/// The callable part of a frozen pi `ToolRegistration`. The returned blocks
/// become the model-visible tool result; cancellation reaches the physical tool.
using ToolExecuteHook = std::move_only_function<support::AsyncResult<std::vector<ai::Content>>(
        support::JsonValue, std::stop_token)>;

struct ToolRegistration {
    ai::Tool declaration{};
    ToolExecuteHook execute{};
};

/// A constant prompt section (frozen pi `section`). Dynamic renderers and
/// wrapping hooks are separate registry capabilities.
struct PromptSection {
    std::string key{};
    std::string text{};
    bool tag{true};
};

struct Extension {
    std::string name{};
    std::vector<ToolRegistration> tools{};
    std::vector<PromptSection> sections{};
};

/// Live, append-only registry for the bounded Harness workflow. Install and
/// request snapshots are serialized internally. Existing registrations stay
/// alive for a request even when another extension is installed.
class Registry {
public:
    Registry();
    Registry(Registry&&) noexcept;
    Registry& operator=(Registry&&) noexcept;
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    [[nodiscard]] support::ExpectedVoid install(Extension extension);

private:
    friend class WorkflowRuntime;

    struct Snapshot {
        /// Shared immutable registrations keep move-only execute operations
        /// alive for every request that captured this snapshot.
        std::vector<std::shared_ptr<ToolRegistration>> tools{};
        std::vector<PromptSection> sections{};
    };
    [[nodiscard]] Snapshot snapshot() const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Stream options selected by the host; no credentials are stored in Session
/// state. Production callers supply `ai::Models::stream` through the AI-owned
/// factory; tests use the same ModelStream contract.
struct HarnessSettings {
    ai::SimpleStreamOptions stream{};
};

struct RootOptions {
    ai::Model model{};
    std::optional<ai::ThinkingLevel> thinking_level{std::nullopt};
};

/// Admission of one foreground input. This bounded workflow uses the durable
/// generation task as its receipt identity; queued/deduplicated Submission
/// storage and inbox policies are not implemented by this handle.
struct Submission {
    TaskId id{};
    ConversationId conversation_id{};
    EntryId entry{};
};

struct SettledSubmission {
    TaskId id{};
    ConversationId conversation_id{};
    EntryId entry{};
    std::optional<EntryId> answer{std::nullopt};
    std::string status{};
    std::optional<std::string> error{std::nullopt};
};

} // namespace cch::durable
