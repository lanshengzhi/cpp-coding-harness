# 04 — Implement `/exit` and text-frontend `/clear`

**What to build:** Add `/exit` as a true registry alias and implement `/clear` at the presentation seam so structured modes never receive terminal control bytes.

**Blocked by:** 02 — Add registry-owned command aliases.

**Status:** implemented

- [x] Register `/exit` as an alias of `/quit`; keep the existing `shutdown_requested` result behavior.
- [x] Make `/quit` and `/exit` terminate text, JSON, and RPC frontends after their terminal result is emitted, as specified by the PRD mode matrix; preserve and display the shutdown handler text where that mode carries human-readable messages.
- [x] Add the existing `"shutdown"` result code to the public `PromptResult` documentation without changing the result shape.
- [x] Register canonical `/clear` with metadata and a no-op command handler for non-text modes.
- [x] The `/clear` handler returns `Usage: /clear` when arguments are present.
- [x] Intercept exact `/clear` only in text frontend code for both REPL and one-shot text mode.
- [x] Emit `\033[2J\033[H`, flush output, and do not call `AgentSession::prompt()` for the intercepted text command.
- [x] Do not call `std::system`, a shell, or an execution environment.
- [x] Ensure JSON and RPC consume `/clear` as `command_handled` with an empty message and no ANSI bytes.
- [x] Add CLI smoke tests for text REPL, one-shot text, JSON, and RPC behavior, including shutdown display text and ANSI absence in structured output.
- [x] In the RPC shutdown test, send a record after `/exit` and prove it is not processed after the shutdown terminal record.
- [x] Run focused coding-agent, CLI, SDK, and architecture tests.

## Comments

Implemented in commit `5992aea`.

Completed validation:

- `cmake --build build -j2`
- `./build/cpp_harness_tests "[coding_agent][prompt]"` — 42 tests passed
- `./build/cpp_harness_tests "[cli][commands]"` — 6 tests passed
- `./build/cpp_harness_tests "[cli][json][commands]"` — 3 tests passed
- `./build/cpp_harness_tests "[cli][rpc][commands]"` — 3 tests passed
- `./build/cpp_harness_tests "[coding_agent]"` — 150 tests passed
- `./build/cpp_harness_tests "[cli]"` — 71 tests passed
- `./build/cpp_harness_tests "[sdk]"` — 48 tests passed
- `./build/cpp_harness_tests "[architecture]"` — 17 tests passed
- `./build/cpp_harness_tests` — 567 tests passed
- `git diff --check`

Code review:

- Standards review found no documented code or architecture violations. It noted low-priority string-code and small duplication smells; the result-code shape is explicitly unchanged by the PRD, and extracting abstractions for two exact frontend checks or test setup was not justified in this slice.
- Spec review found `/exit`, shutdown propagation, frontend termination, `/clear`, ANSI isolation, and RPC stop behavior correct. Its README finding is intentionally deferred to Ticket 05, which owns README and final Phase 1 acceptance alignment.
