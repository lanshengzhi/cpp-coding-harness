# Spec #882 acceptance — MCP + codemode full parity with pi v1.0.4

**Date:** 2026-10-08 (close-out #886, branch `pike/mcp-codemode-882`).
**Authority:** frozen `pi-v1.0.4` evidence bundle captured in #883
(`fixtures/pi-ai/v1.0.4/mcp-codemode/`, baseline `7c10bd4337495ee613f2224843ecdf349b80d1df`, ADR 0065)
plus the implementation notes `notes/882-mcp-codemode-parity/{mcp,codemode}-surface.md`. Per the ADR 0066
full-parity ruling item 3, a story closes only against **differential/bundle evidence**, not
self-captured goldens alone.

**Verdict:** all ten user stories close. Every closure carries bundle-diffed evidence; the only
remaining differences are the pi-pointered **Deferred rows in ADR 0066** (each named there with its
exact pi source), recorded by the #886 sweep. Nothing unrecorded remains.

---

## 1. User stories

### Story 1 — TUI `/mcp` panel and `pike mcp` subcommand manage servers (add/remove/edit, start/stop, login), pi-identical — **Closed**

| Evidence | Where |
|---|---|
| `mcp.json` write half (update/add/remove, default-key deletion, indentation-preserving rewrite) | `McpConfigWriteTest` (9 cases, `[issue884][spec]`); ADR 0066 #884 row |
| Live manager actions (enable/disable, reconnect, exposure set, sign-in/out) over pi's `ServerState` vocabulary | `McpSessionManagerTest` (`an enable persists…`, `reconnect restores a failed server…`, `an exposure change re-registers…`, `disabling a server hides every tool it registered`, `sign-in runs the flow and reconnects`); ADR 0066 #884 row |
| `/mcp` slash wiring: pi's catalog entry, argument completions, panel loop, verbatim status lines, sign-in screen, non-TUI `formatStatus` | `McpManagerFlowTest` (41 cases, `[tui][mcp]`), `EditorAutocompleteTest` (`[autocomplete][issue884]`), `McpSlashCommandInteractiveTest` (4 e2e cases, `[e2e][issue884]`); ADR 0066 slash-wiring revision |
| `pike mcp` CLI subcommands (`add`/`remove`/`list [--json]`/`login`/`logout`, exact pi output lines) | `tests/cli/McpCommandTest.cpp` (~21 cases, `[cli][mcp][issue884]`); ADR 0066 #884 row |

### Story 2 — OAuth login has a user-reachable trigger, pi-identical — **Closed**

| Evidence | Where |
|---|---|
| Sign-in over the flow + `mcp-auth.json`, reconnect after | `McpOAuthSessionTest` (`MCP OAuth login reuses the login surface and persists into mcp-auth.json`); `McpSessionManagerTest` `sign-in runs the flow and reconnects` |
| TUI trigger: `/mcp login [server]`, panel sign-in action, sign-in screen with paste-URL fallback | `McpManagerFlowTest` login/sign-in cases; `McpSlashCommandInteractiveTest`; ADR 0066 slash-wiring revision |
| CLI trigger: `pike mcp login <server> [--timeout]` with pi's outcome lines (already-signed-in, cancelled-with-bound, failed-with-message) | `McpCommandTest` (`mcp login drives the OAuth flow…`, `mcp login reports a cancelled sign-in with pi's timeout bound` — #866 wording fix); pi pointer `extensions/mcp/cli.ts` `login` |
| #865 follow-up absorption: the #875-recorded "user-visible sign-in trigger" deferral | ADR 0066 #875 ruling update ("delivered by #884") |

### Story 3 — Cancelling a remote call sends `notifications/cancelled`, pi-identical — **Closed**

| Evidence | Where |
|---|---|
| Cancel on abort and on timeout, reason `Aborted` / `Request timed out`, never for `initialize`, fire-and-forget | `McpHttpSessionTest` `cancelling an in-flight MCP HTTP call tells the server with notifications/cancelled`; stdio lane `write_cancellation`; diffed against `mcp-protocol-surface.json`; ADR 0066 #884 row |
| Recorded nuance (pi-pointered): the TUI sign-in timeout's classification/bound | ADR 0066 #886 row (not part of this story's call-cancel surface) |

### Story 4 — streamable-http server→client GET stream, pi-identical — **Closed**

| Evidence | Where |
|---|---|
| GET stream after `notifications/initialized`, 405 = feature-absent, `retry:` override, backoff with healthy-stream reset, exhausted-retries error, close semantics, server notifications/ping/unknown-method handling | `McpHttpGetStreamTest` (8 cases); pi pointer `packages/mcp/src/transports/streamable-http.ts`; ADR 0066 #884 row |

### Story 5 — MCP resource tools available, pi-identical — **Closed**

| Evidence | Where |
|---|---|
| `list_mcp_resources` / `list_mcp_resource_templates` / `read_mcp_resource` with pi's verbatim descriptions, parameters, output schemas, multi-server listing, cursor rules, error texts | `McpResourceToolsTest` (9 cases, `[issue884][spec]`); ADR 0066 #884 row |
| Registration at the widest non-hidden exposure over the live connections | `McpSessionManagerTest` `the resource tools take the widest non-hidden exposure…`; integration cases `a direct-exposure server declares its tools and the resource tools read the scripted server's resources`, `a codemode-exposure resource-bearing server keeps the resource tools undeclared but reachable from scripts` |

### Story 6 — Tool exposure policy (codemode/deferred/hidden, and direct) configurable, pi-identical — **Closed**

| Evidence | Where |
|---|---|
| `exposure`/`toolExposure` read half: values, `codemode-deferred` alias, exact-name-beats-pattern | `McpConfigSurfaceTest` (`validateMcpServerConfig matches the pi-v1.0.4 exposure cases`, `getMcpToolExposure matches the pi-v1.0.4 exact-beats-pattern rule`); bundle `mcp-config-surface.json` |
| Activation model: `ensureDiscoveryActive` before any connect and after every action, verbatim warning, `defaultActive:false` codemode reconciliation, withdrawal-as-hidden, resource-tool exposure sync | `McpSessionManagerTest` (9 activation cases, `[issue884][spec]`); ADR 0066 activation-model revision |
| End-to-end declared-set separation | `McpSessionIntegrationTest` (`a codemode-exposure server registers its tools undeclared…`, `autoEnableCodemode false keeps codemode inactive and records pi's verbatim warning…`, `a hidden-exposure server registers its tools undeclared…`); snapshot case `codemode is registered but undeclared until an explicit activation names it` |
| Recorded residual (membership ruling, pi-pointered): `deferred` tools unreachable without `tool_search` | ADR 0066 tool_search Deferred row — `packages/coding-agent/src/extensions/tool-search/tool.ts` |

### Story 7 — Model writes codemode scripts inline (inline `codemode` tool, grammar constraint emission), pi-identical — **Closed**

| Evidence | Where |
|---|---|
| Tool definition (name, `model-only` exposure, single `code` argument, verbatim description/globals, prompt snippet + guideline) | `CodemodeParityTest` `the model-facing codemode tool definition matches the pi-v1.0.4 bundle`; bundle `codemode-tool.json` |
| Grammar emission: `constrainedSampling.variants.openai_lark` is pi's frozen grammar, byte-exact | `CodemodeParityTest` `the codemode grammar variant is pi's frozen CODEMODE_SOURCE_GRAMMAR`; bundle `codemode-source-grammar.lark`; ADR 0033 grammar rows (Supported, `74a1f8315`) |
| Exactly one inline tool; the #870 disk declaration face physically removed (removal evidence: a stray `.pi/codemode` adds nothing) | `CodemodeParityTest` `the codemode tool source contributes exactly pi's one inline tool`, `a stray .pi/codemode declaration adds nothing to the session tool surface`; deletion commits `883db3d59`/`74a1f8315`; ADR 0066 #885 record |
| `@options` parsing (`max_output_tokens`, `timeout_ms`) | `CodemodeSource` + sandbox option tests; pi `source.ts` |

### Story 8 — Scripts orchestrate session tools (`tools.*` routing), pi-identical — **Closed**

| Evidence | Where |
|---|---|
| `tools.<jsName>` and `tools["raw"]` bindings run the session tool; explicit rejection of unlisted names | `CodemodeRoutingTest` (first three cases, `[issue885][spec]`); `CodemodeSandboxTest` `tools.* calls route back to the host tool handler` |
| Nested-call seam on the Agent Owner Interface with pi's records (`{callerId}/{n}`, 256-call cap, per-call/total argument caps, 500-char error) | `tests/agent/NestedToolCallsTest.cpp` (`[agent][codemode][issue885][spec]`); pi pointers `core/nested-tool-calls.ts`, `agent-session.ts` |
| Script-value shape: whole CallToolResult object under pi's output-schema guard, error results included, `Tool "<name>" failed` fallback | `CodemodeRoutingTest` four `[issue886][spec]` cases; `convert_mcp_tools_call_result` scriptResult; pi pointers `execute.ts toScriptValue`, `tools.ts convertMcpResult`; ADR 0066 #885/#886 rows |
| Discovery globals `ALL_TOOLS`/`searchTools`/`describeTool`/`describeNamespace` | `CodemodeDiscovery` + `CodemodeSandboxTest` cases |

### Story 9 — Codemode settings shape in settings.json matches pi — **Closed (with recorded Deferred consumers)**

| Evidence | Where |
|---|---|
| `codemode.mode` / `codemode.inlineBudget` parsed with pi's shape, defaults, project-over-global merge | `SettingsManagerTest` `SettingsManager resolves pi's codemode mode and inline budget`; `Settings.hpp` `UserCodemodeSettings`; pi pointer `core/settings-manager.ts:93-107` |
| `defaultTools`: no Pike counterpart — recorded Deferred, pi-pointered | ADR 0066 settings-keys paragraph (`settings-manager.ts:168`, `:214-253`, `:1433-1435`) |
| Recorded Deferred consumers: `on`/`only` declaration hiding and the budget-capped catalog exist only inside pi's dynamic codemode description, which is its own recorded row | ADR 0066 #885 codemode gaps table (`tool.ts prepareCodemodeLoadout`, `declarations.ts selectCatalog`) |

### Story 10 — Scripts run on a dedicated worker thread; the install ships the script engine — **Closed**

| Evidence | Where |
|---|---|
| Fresh worker thread per run; timeout/interrupt/cancellation; escape fail-closed | `CodemodeSandboxTest` (`an over-limit script is interrupted with a timeout error`, `a pre-cancelled run never starts the script`, `an escape attempt fails closed…`); pi `runtime/host.ts`; ADR 0066 #885 row |
| Guest wasm shipped with the installed Runtime at `share/pike/codemode/quickjs.wasm`, no resolution env var (pi `getQuickJSWasmPath` shape) | `InstallRelocationTest` staged-install exact-file list (`share/pike/codemode/quickjs.wasm`); ADR 0039 amendment (`74a1f8315`); ADR 0066 #885 row |

---

## 2. Testing Decisions (spec §Testing Decisions)

| Decision | Disposition |
|---|---|
| Each capability's acceptance = differential evidence against the frozen `pi-v1.0.4` bundle; self-captured goldens alone are not acceptance | Bundle `fixtures/pi-ai/v1.0.4/mcp-codemode/` (provenance-checked by `tests/ai/McpCodemodeBundleTest.py` and `scripts/ai/check_mcp_codemode_provenance.py`); differential consumers: `CodemodeParityTest` (tool definition + grammar vs `codemode-tool.json`/`codemode-source-grammar.lark`), `McpConfigSurfaceTest` (config/validation/exposure vs `mcp-config-surface.json`), `McpAuthStoreTest` (`the mcp-auth.json key format and file shape match the pi-v1.0.4 bundle`), protocol behavior vs `mcp-protocol-surface.json`, tool naming/schema vs `mcp-tool-surface.json` |
| Security boundaries stay fail-closed (sandbox escape, credentials, TLS-only) | TLS-only: `McpHttpSessionTest` `a non-TLS MCP HTTP URL is rejected at registration`, `an HTTP redirect to a non-TLS target is refused`; credentials: `McpOAuthSessionTest` `a missing MCP OAuth credential is an explicit re-login error, never an unauthenticated request`, `an invalid_grant refresh is an explicit re-login error with no retry loop`; sandbox: `CodemodeSandboxTest` `an escape attempt fails closed and writes nothing to the host filesystem` |
| Disk-declaration removal evidenced by the tool-surface enumeration against pi | `CodemodeParityTest` `a stray .pi/codemode declaration adds nothing to the session tool surface`; physical deletion in `883db3d59`/`74a1f8315` (fixtures, loader, trust gate, tests); ADR 0066 #885 record |

---

## 3. Recorded residuals (all pi-pointered in ADR 0066; nothing unrecorded remains)

1. `tool_search` membership row — `deferred` exposure tools unreachable (`tool-search/tool.ts`).
2. stdio stderr tail (`runtime.ts stderrTail`; `transports/stdio.ts` 64 KB ring) — manager seam landed, production stdio source drains to `/dev/null`.
3. External sign-in pickup (`index.ts turn_start reconnectSignedIn`).
4. Background connect + 10 s first-prompt wait (`index.ts session_start`, `DEFAULT_STARTUP_WAIT_MS`) — Pike keeps the #876 synchronous creation-result contract.
5. Multi-candidate `ui.select` (`index.ts pickServer`).
6. TUI sign-in timeout bound/classification (`callback.ts` 5-minute expiry error vs one 300 s timer).
7. MCP result conversion presentation: 20 KB middle-truncation + temp file (`tools.ts limitMcpContent`, `truncate.ts truncateMiddle`, `output-files.ts writeOutputFile`), resource-link/binary rendering (`tools.ts toModelContent`), renderer details channel (`convertMcpResult`).
8. Dynamic codemode description catalog (`tool.ts createCodemodeDescription`/`prepareCodemodeLoadout`, `declarations.ts selectCatalog`) — incl. the consuming surface for the parsed `codemode.mode`/`inlineBudget`.
9. pi settings key `defaultTools` (`settings-manager.ts:168`) — no Pike counterpart.
10. `models.*` codemode globals — conditional on classifier/image-generation rows that remain `No decision`.

Full row text: `docs/adr/0066-pi-capability-scope-decisions-and-open-questions.md` (#884 implementation
record and revisions, #885 codemode record, #886 close-out sweep).
