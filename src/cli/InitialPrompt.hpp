#pragma once

#include <cch/ai/Content.hpp>
#include <cch/util/Error.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace cch::cli {

/// pi `buildInitialMessage` input for non-interactive mode: the positional
/// messages, the `@file` arguments, the (already trimmed) piped stdin content,
/// and the workspace the file arguments resolve against.
struct InitialMessageInput {
    /// Positional messages in CLI order (pi `parsed.messages`).
    std::vector<std::string> messages;
    /// `@file` argument paths (pi `parsed.fileArgs`).
    std::vector<std::string> file_arguments;
    /// The workspace that relative file arguments resolve against.
    std::filesystem::path working_directory;
    /// Piped stdin content, trimmed like pi `readPipedStdin`; empty means
    /// absent and contributes nothing to the merge.
    std::string stdin_content;
};

/// pi `buildInitialMessage` result: the merged initial prompt plus the
/// remaining positional messages that prompt sequentially afterwards.
struct InitialMessageResult {
    /// Piped stdin + `@file` text + the first positional message, joined with
    /// no separator (pi `parts.join("")`). Empty when no part is present.
    std::string initial_message;
    /// Image content extracted from the `@file` arguments.
    std::vector<ai::ImageContent> initial_images;
    /// Positional messages after the first one (pi `parsed.messages` after
    /// `shift()`); each prompts sequentially and the last response is output.
    std::vector<std::string> remaining_messages;
};

/// Combine piped stdin content, `@file` text, and the first CLI message into
/// a single initial prompt for print mode, exactly like pi
/// `buildInitialMessage` (pi `src/cli/initial-message.ts`): stdin first, then
/// file text, then the first positional, concatenated with no separator. The
/// first positional is consumed; the rest are returned for sequential
/// prompting. File processing follows pi `processFileArguments`: text files
/// become `<file name="...">` references, images are sniffed and attached as
/// image content, empty files are skipped.
[[nodiscard]] util::Expected<InitialMessageResult> build_initial_message(
    const InitialMessageInput& input);

} // namespace cch::cli
