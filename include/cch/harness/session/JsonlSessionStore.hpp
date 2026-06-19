#pragma once

#include "SessionEntry.hpp"

#include "../../util/Error.hpp"
#include "../../util/JsonValue.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::harness::session {

class JsonlSessionStore {
public:
    JsonlSessionStore() = default;

    static util::Expected<JsonlSessionStore> create_new(const std::filesystem::path& path, SessionMetadata metadata);
    static util::Expected<JsonlSessionStore> open_existing(const std::filesystem::path& path);
    static util::Expected<LoadedSession> load(const std::filesystem::path& path);

    [[nodiscard]] util::ExpectedVoid append(const ai::MessageVariant& message);

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
                                                                  std::string content,
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
                                                        std::optional<bool> from_hook);
    [[nodiscard]] util::ExpectedVoid append_branch_summary(std::optional<std::string> parent_id,
                                                            std::string from_id,
                                                            std::string summary,
                                                            std::optional<util::JsonValue> details,
                                                            std::optional<bool> from_hook);
    [[nodiscard]] util::ExpectedVoid append_session_info(std::optional<std::string> parent_id,
                                                          std::string name);

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] const SessionMetadata& metadata() const { return metadata_; }

private:
    static util::ExpectedVoid validate_session_path_for_open(const std::filesystem::path& path, bool must_exist);
    static util::ExpectedVoid ensure_private_permissions(const std::filesystem::path& path, bool existing);

    std::filesystem::path path_;
    SessionMetadata metadata_;
    std::size_t next_entry_id_{1};
};

} // namespace cch::harness::session
