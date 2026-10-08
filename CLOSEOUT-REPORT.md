# Close-out report — #886 (spec #882: MCP + codemode full parity with pi v1.0.4)

**Branch:** `pike/mcp-codemode-882` (base tip `bfd3ae244`, an amend of `7c031e34f` — identical modulo
a formatting pass on four TUI/test files).
**Method:** full-row sweep of every MCP/codemode capability surface in ADR 0066 against the landed
branch, the frozen `pi-v1.0.4` bundle (`fixtures/pi-ai/v1.0.4/mcp-codemode/`), and the implementation
notes (`notes/882-mcp-codemode-parity/*.md`); the ACTIVATION-REPORT's Remaining-gaps sections were the
starting suspect list. Authority for disputed details was the pi checkout at `7c10bd43`.

## Verdict

**No unrecorded MCP/codemode behaviour difference remains.** Every residual found in the sweep is
either fixed in this close-out or recorded as a Deferred row in ADR 0066 with its exact pi pointer.

The capability surfaces enumerated and confirmed against landed reality: MCP transports
(stdio + streamable-http, TLS-only), the JSON-RPC protocol (notifications/cancelled, GET stream,
ping/roots, pagination), tool conversion (`mcp__<server>__<tool>` + hash suffix, result schema), the
three resource tools, the exposure policy and activation model (ensureDiscoveryActive,
defaultActive:false, verbatim warning, withdrawal-as-hidden), `mcp.json` global+project
persistence and write half, the `/mcp` panel + `pike mcp` CLI, OAuth (RFC 9728 discovery, DCR,
PKCE, CIMD, mcp-auth.json, auth.json migration, sign-in triggers), lifecycle states, the
`mcp_servers` prompt section; codemode (inline tool + grammar constraint emission, source.ts
@options, tools.* routing + nested calls, worker-thread execution, guest packaging, text|image
output items, settings keys).

## Fixed in this close-out (2 items, both small and focused)

1. **OAuth sign-in cancellation wording** (slash-wiring residual): the production driver now returns
   pi's info-level `Sign-in cancelled.` for the cancelled error (`extensions/mcp/index.ts:628`), so
   the landed /mcp flow's pi check matches; other failures keep the manager's pi-verbatim
   `Sign-in failed: <message>`. The `pike mcp login` CLI now renders pi `login`'s two failure lines
   (`was cancelled or not completed within N seconds.` / `failed: <message>`,
   `extensions/mcp/cli.ts`).
2. **Codemode script value shape** (ACTIVATION-REPORT residual 6): a script call now resolves to
   pi's whole CallToolResult object minus `_meta` under pi's output-schema guard — error results
   included (`isError` preserved, MCP error results inspectable instead of thrown), text otherwise,
   with pi's `Tool "<name>" failed` fallback (`execute.ts toScriptValue`, `tools.ts
   convertMcpResult`). Four separation cases added to `CodemodeRoutingTest`.

## Recorded as Deferred in ADR 0066 (10 pi-pointered rows, no other differences remain)

tool_search membership row · stdio stderr tail · external sign-in pickup · background-connect
nuance · multi-candidate `ui.select` · TUI sign-in timeout bound/classification · MCP result
conversion presentation (20 KB truncation + temp file, resource-link/binary rendering, renderer
details channel) · dynamic codemode description catalog (the consuming surface for the parsed
`codemode.mode`/`inlineBudget`) · `defaultTools` (no Pike counterpart) · `models.*` globals
(conditional on No-decision model rows).

The #865 absorbed follow-ups each carry their absorbing pointer in ADR 0066 (script→tool routing,
worker thread, guest packaging → #885; `/mcp` write half, login trigger, RFC 9728, resource
tools, GET stream, exposure policy → #884).

## Documentation landed

- `docs/research/spec-882-acceptance.md` — all ten user stories + Testing Decisions mapped to
  differential/bundle evidence; every story closes.
- ADR 0066 — slash-wiring delivery record, #885 codemode implementation record, #886 close-out
  revision, corrected settings-keys paragraph, refreshed package rows/Consequences.

## Gates (this tip, after the close-out edits)

- `ctest --preset vcpkg -L architecture`: **41/41** (incl. the -Werror warning gate and the parity
  gate; PARITY-4011 include evidence re-scanned after the close-out include-set changes).
- `bash scripts/format-check.sh 513290fc3`: **clean**.
- Focused: `codemode` 33/33, `mcp` 224/224, TUI/e2e/CLI MCP selections 16/16 and 47/47. Full suite
  was already green at the base tip (2904/2904, mcp|codemode|tui 1176/1176) and was not re-run per
  the close-out instruction.

## Commits (this close-out, oldest first)

| Commit | Message |
|---|---|
| `5c662b4fc` | `fix(mcp): align MCP sign-in outcomes with pi's messages (issue 886)` |
| `813eaa52d` | `fix(codemode): resolve script calls to pi's CallToolResult object (issue 886)` |
| `811e4cfdc` | `docs(adr): sweep ADR 0066 to the landed full-parity reality (issue 886)` |
| `4e23dbe2e` | `docs(research): land the spec 882 acceptance table (issue 886)` |
| (this file) | `docs: append the 886 close-out report (issue 886)` |

## REVIEW-FIXES

All 12 code-review findings identified during close-out have been addressed with dedicated TDD loops, individual conventional commits, and focused test verification:

| Finding | Summary | Root Cause | Fix Commit | Test Evidence |
|---|---|---|---|---|
| **1 (spec)** | MCP `initialize` protocol version | Defaulted to outdated protocol without validating negotiated version against bundle | `6e5ea43b3` | `McpProtocolSurfaceTest.cpp` (4 test cases consuming bundle `mcp-protocol-surface.json`) |
| **2 (spec)** | `tools/list` pagination bounds and guards | Unbounded pagination loop; missing duplicate cursor detection | `d21ffaab6` | `McpToolsListTest.cpp` (5 cases with scripted connection double; dup cursor hung red, passes green) |
| **3 (spec)** | Tool-name length (64) & hash-suffix collision rule | `mcp_tool_name` truncated without hashing; order-dependent naming | `5fe290acb` | `McpToolNameTest.cpp` (5 cases reproducing bundle `mcp-tool-surface.json` examples) |
| **4 (spec)** | Progress notifications & deadline re-arming | Requests lacked `_meta.progressToken`; no progress handler or timeout re-arming | `884298e30` | `McpProgressTest.cpp` (new file with 6 cases; progress update text formatting, token injection and extraction) |
| **5 (spec)** | HTTP resilience (session expiry retry, connect backoff, SSE resume, roots) | Missing 404 session-expiry retry, connect retry [250, 1000]ms, synthetic -32603 on response stream break, roots capability & handler | `7aa91e16c` | `McpHttpGetStreamTest.cpp` (roots/list answered with file://<cwd>), `McpProtocolSurfaceTest.cpp` (capabilities advertises roots) |
| **6 (P0)** | SSE retry field parsing with `std::from_chars` | Used `std::stoi` on server-controlled SSE `retry:` field which throws on overflow | `dc6a2d6c0` | `McpHttpGetStreamTest.cpp` ("out-of-range server retry field is ignored, never throws") |
| **7 (P1)** | Coroutine lambda captures in `McpCommand.cpp` | Used implicit `[&]` captures across coroutine suspend points (§6.2) | `605765384` | `ctest -R 'mcp login\|mcp logout'` (13/13 passing) |
| **8 (P1)** | Convert single-use hooks to `std::move_only_function` | Used `std::function` for single-use CLI callbacks (`McpSignInHook`, `open_browser`) | `5b7f2a580` | `ctest -R 'mcp login\|mcp logout'` (13/13 passing) |
| **9 (P2)** | `compare(0, ...)` → `starts_with` | Did not use standard C++20 `starts_with` per §9.2 | `501faaf92` | Stdio focused tests |
| **10 (P2)** | Hand-rolled 1ms poll → `tests::ReleaseGate` | Hand-rolled busy poll loop in test instead of synchronization primitive (§11.8) | `165d8abd5` | `McpHttpGetStreamTest.cpp` (10/10 passing) |
| **11 (P1)** | 4× duplicated URL authority parser | Duplicate hand-rolled URL authority and host-port parsers across 4 MCP files | `1e291894b` | `McpUrl.hpp/cpp` extracted; `OAuth*` and `Config*` tests (61/61 passing) |
| **12 (P2)** | Stale doc debt comment & paste-reader thread lifetime | Outdated comment about synchronous runs; detached thread could post to destroyed context | `7ccb287a3` | `CodemodeSandbox.hpp` updated; lifetime contract documented in `McpCommand.cpp` |

