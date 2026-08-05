#pragma once

#include "SessionEntry.hpp"
#include "SessionStore.hpp"

#include "../../util/Error.hpp"
#include "../../util/JsonValue.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <cstring>
#include <vector>

namespace cch::harness::session {

class SessionTree;

class JsonlSessionStore final : public SessionStore {
public:
    struct Impl;
public:
    JsonlSessionStore() = default;
    ~JsonlSessionStore() override;
    JsonlSessionStore(JsonlSessionStore&&);
    JsonlSessionStore& operator=(JsonlSessionStore&&);

    static util::Expected<JsonlSessionStore> create_new(const std::filesystem::path& path, SessionMetadata metadata);
    static util::Expected<JsonlSessionStore> open_existing(const std::filesystem::path& path);
    static util::Expected<LoadedSession> load(const std::filesystem::path& path);

    /// Load a session and construct a SessionTree for navigation.
    static util::Expected<SessionTree> open_as_tree(const std::filesystem::path& path);

    [[nodiscard]] util::ExpectedVoid append(const ai::MessageVariant& message) override;

    // --- v3 tree entry append methods ---
    [[nodiscard]] util::ExpectedVoid append_model_change(std::optional<std::string> parent_id,
                                                          std::string provider,
                                                          std::string model_id);
    [[nodiscard]] util::ExpectedVoid append_thinking_level_change(std::optional<std::string> parent_id,
                                                                   std::string thinking_level);
    [[nodiscard]] util::ExpectedVoid append_active_tools_change(std::optional<std::string> parent_id,
                                                                 std::vector<std::string> tools);
    [[nodiscard]] util::ExpectedVoid append_custom_entry(std::optional<std::string> parent_id,
                                                          std::string custom_type,
                                                          util::JsonValue data);
    [[nodiscard]] util::ExpectedVoid append_custom_message_entry(std::optional<std::string> parent_id,
                                                                  std::string custom_type,
                                                                  CustomMessageEntryContent content,
                                                                  bool display,
                                                                  std::optional<util::JsonValue> details);
    [[nodiscard]] util::ExpectedVoid append_label_change(std::optional<std::string> parent_id,
                                                          std::string target_id,
                                                          std::optional<std::string> label);
    [[nodiscard]] util::ExpectedVoid append_compaction(std::optional<std::string> parent_id,
                                                        std::string summary,
                                                        std::string first_kept_entry_id,
                                                        std::size_t tokens_before,
                                                        std::optional<util::JsonValue> details,
                                                        std::optional<bool> from_hook,
                                                        std::vector<ai::MessageVariant> retained_tail = {},
                                                        std::optional<ai::Usage> usage = std::nullopt);
    [[nodiscard]] util::ExpectedVoid append_branch_summary(std::optional<std::string> parent_id,
                                                            std::string from_id,
                                                            std::string summary,
                                                            std::optional<util::JsonValue> details,
                                                            std::optional<bool> from_hook);
    [[nodiscard]] util::ExpectedVoid append_session_info(std::optional<std::string> parent_id,
                                                          std::string name);

    /// Write a Leaf entry to persist the current active leaf position.
    /// The target_id is the entry ID that becomes the new leaf.
    [[nodiscard]] util::ExpectedVoid append_leaf(std::optional<std::string> parent_id,
                                                  std::string target_id);

    [[nodiscard]] std::optional<std::filesystem::path> path() const override;
    [[nodiscard]] const SessionMetadata& metadata() const;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::harness::session
