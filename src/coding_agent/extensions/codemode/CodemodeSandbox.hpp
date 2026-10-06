#pragma once

#include <cch/support/Error.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

/// One item a codemode script emitted with `text()`, `image()`, or `console.*`
/// (pi `CodemodeOutputItem`). The kind is the pi channel, not a rendering hint.
struct CodemodeOutputItem {
    enum class Kind { Text, Image };
    Kind kind{Kind::Text};
    /// `Text`: the rendered text. `Image`: the base64 image data (no `data:`
    /// prefix), matching pi's `{ type: "image", data, mimeType }`.
    std::string data;
    /// `Image` only: the detected MIME type (pi detects it from the base64
    /// signature; the type in the data URI is ignored).
    std::string mime_type;
};

/// Why a run did not produce a value (pi `CodemodeError`): a script-thrown
/// error, a sandbox-boundary failure, a timeout, or a host cancellation.
struct CodemodeError {
    enum class Kind { Script, Sandbox, Timeout, Aborted };
    Kind kind{Kind::Sandbox};
    /// The user-visible message. A script error is the prelude's `Name: message`
    /// text; a sandbox error names the boundary that failed.
    std::string message;
};

/// pi `CodemodeResult`: the terminal outcome of one script run. `error` unset
/// means success and `value_json` (when present) holds the script's return
/// value as JSON text.
struct CodemodeRunResult {
    std::vector<CodemodeOutputItem> output;
    std::optional<std::string> value_json;
    std::optional<CodemodeError> error;
};

/// pi `CodemodeSandboxOptions`/`CodemodeExecuteOptions` bounds the sandbox can
/// enforce from outside the guest.
struct CodemodeLimits {
    /// pi's default execution timeout (300 s). The VM's `host_interrupt` import
    /// polls this; a script that exceeds it is interrupted and reported as a
    /// timeout. Zero disables the timeout.
    std::chrono::milliseconds timeout{std::chrono::seconds{300}};
    /// QuickJS heap cap in bytes; zero keeps the guest default.
    std::size_t memory_bytes{0};
};

/// Executes one tool a script requested through `tools.*` or a global, reusing
/// the host's existing tool implementations (and therefore their containment).
/// `arguments_json` is the JSON text of the call argument, or an empty string
/// when the call passed no argument. The result is the tool's JSON text, or an
/// error the sandbox reports to the script as a rejected call.
using CodemodeToolCallHandler =
        std::function<support::Expected<std::string>(std::string_view name, std::string_view arguments_json)>;

/// One tool the script may call through `tools.<js_name>` (pi `CodemodeTool`).
/// The host lists these into the guest's `toolsJson`, so a call routes back
/// through `CodemodeToolCallHandler`.
struct CodemodeToolDescriptor {
    /// The tool's registered name (`tools[name]` in the script).
    std::string name;
    /// The identifier form (`tools[jsName]`); the caller derives it as pi's
    /// `toCodemodeIdentifier` does.
    std::string js_name;
    std::string description;
};

/// The codemode wasm sandbox (spec #865, ticket #874). It loads pi's actual
/// codemode guest (QuickJS compiled to wasm) through WasmEdge, registers only
/// the twelve imports the guest declares (no filesystem, socket, process, or
/// module capability), evaluates pi's codemode prelude and the declared script
/// inside it, and returns the script's value and output.
///
/// The boundary fails closed: an unregistered import refuses instantiation, and
/// a script can reach the host only through the prelude's `bridge`, which the
/// host routes to `text`/`image`/`console` output, the tool call handler, and
/// `done`. There is no host shell or filesystem path into the guest.
///
/// `run` is synchronous and drives the guest on the calling thread; the VM's
/// interrupt import is polled so a `stop_token` or the timeout can end a
/// runaway script. One sandbox owns one guest module; `run` instantiates a
/// fresh instance per call, so a crashed or interrupted guest never poisons a
/// later run.
class CodemodeSandbox {
public:
    /// Parse and validate the guest module at `wasm_path`. A missing or invalid
    /// module is an explicit error; nothing is deferred to the first `run`.
    [[nodiscard]] static support::Expected<std::unique_ptr<CodemodeSandbox>> create(std::filesystem::path wasm_path);

    ~CodemodeSandbox();
    CodemodeSandbox(CodemodeSandbox&&) noexcept;
    CodemodeSandbox& operator=(CodemodeSandbox&&) noexcept;
    CodemodeSandbox(const CodemodeSandbox&) = delete;
    CodemodeSandbox& operator=(const CodemodeSandbox&) = delete;

    /// Run `script` — a codemode source body (pi `parseCodemodeSource` result) —
    /// as an async function body, exactly as pi's worker does, and return its
    /// terminal outcome. `tools` become the script's `tools.*` surface; each
    /// call is resolved through `handler`. A call with no `handler`, or a name
    /// not in `tools`, is a rejected call the script sees — never a silent
    /// success.
    [[nodiscard]] CodemodeRunResult run(std::string_view script,
            const std::vector<CodemodeToolDescriptor>& tools,
            CodemodeLimits limits,
            std::stop_token stop_token = {},
            CodemodeToolCallHandler handler = {});

private:
    struct Impl;
    explicit CodemodeSandbox(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace cch::coding_agent::extensions
