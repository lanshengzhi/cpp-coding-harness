#pragma once

#include <cch/agent/harness/session/SessionEntry.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>

#include <cch/support/Error.hpp>
#include <cch/support/JsonValue.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <cstring>
#include <vector>

namespace cch::harness::session {

class SessionTree;

class JsonlSessionStore final {
public:
    struct Impl;
public:
    JsonlSessionStore() = default;
    ~JsonlSessionStore();
    JsonlSessionStore(JsonlSessionStore&&);
    JsonlSessionStore& operator=(JsonlSessionStore&&);

    static support::Expected<JsonlSessionStore> create_new(const std::filesystem::path& path, SessionMetadata metadata);
    static support::Expected<JsonlSessionStore> open_existing(const std::filesystem::path& path);
    static support::Expected<LoadedSession> load(const std::filesystem::path& path);

    /// Load a session and construct a SessionTree for navigation.
    static support::Expected<SessionTree> open_as_tree(const std::filesystem::path& path);

    [[nodiscard]] support::ExpectedVoid append(const ai::MessageVariant& message);

    // --- v3 tree entry append methods ---
    [[nodiscard]] support::ExpectedVoid append_model_change(std::optional<std::string> parent_id,
                                                          std::string provider,
                                                          std::string model_id);
    [[nodiscard]] support::ExpectedVoid append_thinking_level_change(std::optional<std::string> parent_id,
                                                                   std::string thinking_level);
    [[nodiscard]] support::ExpectedVoid append_active_tools_change(std::optional<std::string> parent_id,
                                                                 std::vector<std::string> tools);
    [[nodiscard]] support::ExpectedVoid append_custom_entry(std::optional<std::string> parent_id,
                                                          std::string custom_type,
                                                          support::JsonValue data);
    [[nodiscard]] support::ExpectedVoid append_custom_message_entry(std::optional<std::string> parent_id,
                                                                  std::string custom_type,
                                                                  CustomMessageEntryContent content,
                                                                  bool display,
                                                                  std::optional<support::JsonValue> details);
    [[nodiscard]] support::ExpectedVoid append_label_change(std::optional<std::string> parent_id,
                                                          std::string target_id,
                                                          std::optional<std::string> label);
    [[nodiscard]] support::ExpectedVoid append_compaction(std::optional<std::string> parent_id,
                                                        std::string summary,
                                                        std::string first_kept_entry_id,
                                                        std::size_t tokens_before,
                                                        std::optional<support::JsonValue> details,
                                                        std::optional<bool> from_hook,
                                                        std::vector<ai::MessageVariant> retained_tail = {},
                                                        std::optional<ai::Usage> usage = std::nullopt);
    [[nodiscard]] support::ExpectedVoid append_branch_summary(std::optional<std::string> parent_id,
                                                            std::string from_id,
                                                            std::string summary,
                                                            std::optional<support::JsonValue> details,
                                                            std::optional<bool> from_hook);
    [[nodiscard]] support::ExpectedVoid append_session_info(std::optional<std::string> parent_id,
                                                          std::string name);

    /// Write a Leaf entry to persist the current active leaf position.
    /// The target_id is the entry ID that becomes the new leaf; nullopt
    /// writes the root leaf (pi `setLeafId(null)`, used after navigating
    /// back before the first entry).
    [[nodiscard]] support::ExpectedVoid append_leaf(std::optional<std::string> parent_id,
                                                  std::optional<std::string> target_id);

    [[nodiscard]] std::optional<std::filesystem::path> path() const;
    [[nodiscard]] const SessionMetadata& metadata() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::harness::session
