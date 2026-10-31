#pragma once

#include <functional>
#include <string>

namespace cch::durable::testing {

/// One physical file operation of a JSONL store. The seam is private and
/// test-only, not a public contract: it observes the operation order and can
/// fail a selected call before it runs, mirroring the frozen `FileSystem`
/// instrumentation of `packages/durable/test/jsonl-storage.test.ts`.
enum class JsonlIoOperation {
    Append,
    Flush,
    Write,
    Rename,
    Remove,
};

struct JsonlIoEvent {
    JsonlIoOperation operation{JsonlIoOperation::Append};
    /// The target file's basename inside the store directory.
    std::string file{};
    /// The rename destination's basename; empty for every other operation.
    std::string destination{};
};

/// Return true to fail this operation before it touches the filesystem. The
/// hook is null-safe when unset and is copied before it runs, so clearing it
/// never waits for a hook already in progress.
using JsonlIoHook = std::function<bool(const JsonlIoEvent&)>;

void set_jsonl_io_hook(JsonlIoHook hook);
void clear_jsonl_io_hook();

/// Run the installed hook for one physical operation; true means the operation
/// must fail without running.
[[nodiscard]] bool fail_jsonl_io(const JsonlIoEvent& event);

} // namespace cch::durable::testing
